#include "AlignLoads.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Scope.h"
#include "Bounds.h"
#include "ModulusRemainder.h"
#include "Simplify.h"
namespace Halide {
namespace Internal {
using std::vector;

class AlignLoads : public IRMutator {
public:
    AlignLoads(Target t, int vs) : target(t), required_alignment(vs) {}
private:
    Target target;
    // The desired alignment of a vector load.
    int required_alignment;
    // Alignment info for variables in scope.
    Scope<ModulusRemainder> alignment_info;
    using IRMutator::visit;
    ModulusRemainder get_alignment_info(Expr e) {
        return modulus_remainder(e, alignment_info);
    }
    int natural_vector_lanes(Type t) {
        return required_alignment / t.bytes();
    }

    Expr concat_and_shuffle(Expr vec_a, Expr vec_b, std::vector<Expr> &indices) {
        Type dbl_t = vec_a.type().with_lanes(vec_a.type().lanes() * 2);
        Expr dbl_vec = Call::make(dbl_t, Call::concat_vectors, { vec_a, vec_b }, Call::PureIntrinsic);
        std::vector<Expr> args;
        args.push_back(dbl_vec);
        for (int i = 0; i < (int) indices.size(); i++) {
            args.push_back(indices[i]);
        }
        return Call::make(vec_a.type(), Call::shuffle_vector, args, Call::PureIntrinsic);
    }
    Expr concat_and_shuffle(Expr vec_a, Expr vec_b, int start, int size) {
        std::vector<Expr> args;
        for (int i = start; i < start+size; i++) {
            args.push_back(i);
        }
        return concat_and_shuffle(vec_a, vec_b, args);
    }

    bool get_alignment_check(const Ramp *ramp, int host_alignment, Type t, int *lanes_off) {
        // If the start of the buffer is not aligned, we conservatively assume unaligned.
        // This way, we can restrict alignment detection to the index, i.e the ramp itself.
        if (host_alignment % required_alignment) return false;
        int reqd_alignment_lanes = required_alignment / t.bytes();
        return reduce_expr_modulo(ramp->base, reqd_alignment_lanes, lanes_off, alignment_info);
    }
    void visit(const Load *op) {
        debug(4) << "AlignLoads: Working on " << (Expr) op << "..\n";
        Expr index = mutate(op->index);
        if (op->type.is_vector()) {
            bool external = op->image.defined();
            if (external) {
                debug(4) << "AlignLoads: Not dealing with an external image\n";
                debug(4) << (Expr) op << "\n";
                expr = op;
                return;
            } else {
                const Ramp *ramp = index.as<Ramp>();
                const IntImm *stride = ramp ? ramp->stride.as<IntImm>() : NULL;
                // We will work only on natural vectors supported by the target.
                int native_lanes = natural_vector_lanes(op->type);
                if (ramp->lanes < native_lanes) {
                    // Load a native vector and then shuffle.
                    if (stride->value > 2) {
                        // Right now we deal only with strides at most 2.
                        expr = op;
                        return;
                    } else {
                        Expr vec = mutate(Load::make(op->type.with_lanes(native_lanes), op->name,
                                                      Ramp::make(ramp->base, ramp->stride, native_lanes),
                                                      op->image, op->param));
                        std::vector<Expr> args;
                        args.push_back(vec);
                        // We can now pick contiguous lanes because vec should be a shuffle_vector of native length
                        // that picks out the elements needed.
                        for (int i = 0; i < ramp->lanes; i++) {
                            args.push_back(i);
                        }
                        expr = Call::make(op->type, Call::shuffle_vector, args, Call::PureIntrinsic);
                        return;
                    }
                } else if (ramp->lanes > native_lanes) {
                    int load_lanes = ramp->lanes;
                    std::vector<Expr> slices;
                    for (int i = 0; i < load_lanes; i+= native_lanes) {
                        int slice_lanes = std::min(native_lanes, (load_lanes - i));
                        Expr slice_base = simplify(ramp->base + i);
                        Expr slice =
                            Load::make(op->type.with_lanes(slice_lanes), op->name, Ramp::make(slice_base, ramp->stride, slice_lanes),
                                       op->image, op->param);
                        slices.push_back(slice);
                    }
                    expr = mutate(Call::make(op->type, Call::concat_vectors, slices, Call::PureIntrinsic));
                    return;
                } else if (ramp && stride && stride->value == 1) {
                    // If this is a parameter, the base_alignment should be host_alignment.
                    // This cannot be an external image because we have already checked for
                    // it. Otherwise, this is an internal buffers that is always aligned
                    // to the natural vector width.
                    int base_alignment =
                        op->param.defined() ? op->param.host_alignment() : required_alignment;
                    int lanes_off;
                    bool known_alignment = get_alignment_check(ramp, base_alignment, op->type, &lanes_off);
                    if (known_alignment && lanes_off != 0) {
                        int lanes = ramp->lanes;
                        Expr base = ramp->base;
                        Expr base_low = base - (lanes_off);
                        Expr ramp_low = Ramp::make(simplify(base_low), 1, lanes);
                        Expr ramp_high = Ramp::make(simplify(base_low + lanes), 1, lanes);
                        Expr load_low = Load::make(op->type, op->name, ramp_low, op->image, op->param);
                        Expr load_high = Load::make(op->type, op->name, ramp_high, op->image, op->param);
                        // slice_vector considers the vectors in it to be concatenated and the result
                        // is a vector containing as many lanes as the last argument and the vector
                        // begins at lane number specified by the penultimate arg.
                        // So, slice_vector(2, a, b, 126, 128) gives a vector of 128 lanes starting
                        // from lanes 126 through 253 of the concatenated vector a.b.
                        debug(4) << "AlignLoads: Unaligned Load: Converting " << (Expr) op << " into ...\n";
                        expr = concat_and_shuffle(load_low, load_high, lanes_off, lanes);
                        debug(4) <<  "... " << expr << "\n";
                        return;
                    } else {
                        debug(4) <<  "AlignLoads: Unknown alignment or aligned load";
                        debug(4) << "AlignLoads: Type: " << op->type << "\n";
                        debug(4) << "AlignLoads: Index: " << index << "\n";
                        expr = op;
                        return;
                    }
                } else if (ramp && stride && stride->value == 2) {
                    // We'll try to break this into two dense loads followed by a shuffle.
                    Expr base_a = ramp->base, base_b = ramp->base + ramp->lanes;
                    int b_shift = 0;
                    int lanes = ramp->lanes;

                    if (op->param.defined()) {
                        // We need to be able to figure out if buffer_base + base_a is aligned. If not,
                        // we may have to shift base_b left by one so as to not read beyond the end
                        // of an external buffer.
                        int lanes_off = 0;
                        bool known_alignment = get_alignment_check(ramp, op->param.host_alignment(), op->type, &lanes_off);

                        if (!known_alignment || (known_alignment && lanes_off)) {
                            debug(4) << "AlignLoads: base_a is unaligned: shifting base_b\n";
                            debug(4) << "AlignLoads: Type: " << op->type << "\n";
                            debug(4) << "AlignLoads: Index: " << index << "\n";
                            base_b -= 1;
                            b_shift = 1;
                        }
                    }

                    Expr ramp_a = Ramp::make(base_a, 1, lanes);
                    Expr ramp_b = Ramp::make(base_b, 1, lanes);
                    Expr vec_a = mutate(Load::make(op->type, op->name, ramp_a, op->image, op->param));
                    Expr vec_b = mutate(Load::make(op->type, op->name, ramp_b, op->image, op->param));

                    std::vector<Expr> indices;
                    for (int i = 0; i < lanes/2; ++i) {
                        indices.push_back(i*2);
                    }
                    for (int i = lanes/2; i < lanes; ++i) {
                        indices.push_back(i*2 + b_shift);
                    }

                    debug(4) << "AlignLoads: Unaligned Load: Converting " << (Expr) op << " into ...\n";

                    expr = concat_and_shuffle(vec_a, vec_b, indices);

                    debug(4) <<  "... " << expr << "\n";
                    return;
                }else {
                    expr = op;
                    return;
                }
            }
        } else {
            expr = op;
            return;
        }
    }

    template<typename NodeType, typename LetType>
    void visit_let(NodeType &result, const LetType *op) {
        if (op->value.type() == Int(32)) {
            alignment_info.push(op->name, modulus_remainder(op->value, alignment_info));
        }

        Expr value = mutate(op->value);
        NodeType body = mutate(op->body);

        if (op->value.type() == Int(32)) {
            alignment_info.pop(op->name);
        }
        result = LetType::make(op->name, value, body);
    }

    void visit(const Let *op) { visit_let(expr, op); }
    void visit(const LetStmt *op) { visit_let(stmt, op); }
    void visit(const For *op) {
        int saved_required_alignment = required_alignment;
        if (op->device_api == DeviceAPI::Hexagon) {
            if (target.has_feature(Target::HVX_128)) {
                required_alignment = 128;
            }
            else if (target.has_feature(Target::HVX_64)) {
                required_alignment = 64;
            } else {
                internal_error << "Unknown HVX mode";
            }
        }
        IRMutator::visit(op);
        required_alignment = saved_required_alignment;
        return;
    }
};

Stmt align_loads(Stmt s, const Target &t) {
    return AlignLoads(t, t.natural_vector_size(Int(8))).mutate(s);
  }
}
}