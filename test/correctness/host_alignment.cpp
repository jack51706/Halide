#include "Halide.h"
#include <stdio.h>
#include <map>
#include <string>

using std::vector;
using std::map;
using std::string;
using namespace Halide;
using namespace Halide::Internal;
class FindErrorHandler : public IRVisitor {
public:
    bool result;
    FindErrorHandler() : result(false) {}
    using IRVisitor::visit;
    void visit(const Call *op) {
        if (op->name == "halide_error_unaligned_host_ptr" &&
            op->call_type == Call::Extern) {
            result = true;
            return;
        }
        IRVisitor::visit(op);
    }

};

class ParseCondition : public IRVisitor {
public:
    Expr left, right;

    using IRVisitor::visit;
    void visit(const Mod *op) {
        left = op->a;
        right = op->b;
        return;
    }
};
class CountHostAlignmentAsserts : public IRVisitor {
public:
    int count;
    std::map<string, Expr> alignments_needed;
    CountHostAlignmentAsserts(std::map<string, Expr> m) : count(0),
                                                        alignments_needed(m){}

    using IRVisitor::visit;

    void visit(const AssertStmt *op) {
        Expr m = op->message;
        FindErrorHandler f;
        m.accept(&f);
        if (f.result) {
            Expr c = op->condition;
            ParseCondition p;
            c.accept(&p);
            if (p.left.defined() && p.right.defined()) {
                Expr name = p.left;
                Expr alignment = p.right;
                const Variable *V = name.as<Variable>();
                string name_host_ptr = V->name;
                Expr expected_alignment = alignments_needed[name_host_ptr];
                if (equal(alignment, expected_alignment)) {
                    count++;
                }
            }
        }
    }
};
void set_alignment_host_ptr(ImageParam &i, int align, std::map<string, Expr> &m) {
    i.set_host_alignment(align);
    m.insert(std::pair<string, Expr>(i.name()+".host", align));
}
int count_host_alignment_asserts(Func f, std::map<string, Expr> m) {
    Target t = get_jit_target_from_environment();
    t.set_feature(Target::NoBoundsQuery);
    f.compute_root();
    Stmt s = Internal::lower({f.function()}, f.name(), t);
    CountHostAlignmentAsserts c(m);
    s.accept(&c);
    return c.count;
}
int main(int argc, char **argv) {
    Var x, y, c;
    std::map <string, Expr> m;
    ImageParam i1(Int(8), 1);
    ImageParam i2(Int(8), 1);
    ImageParam i3(Int(8), 1);

    set_alignment_host_ptr(i1, 128, m);
    set_alignment_host_ptr(i2, 32, m);

    Func f;
    f(x) = i1(x) + i2(x) + i3(x);
    f.output_buffer().set_host_alignment(128);
    m.insert(std::pair<string, Expr>("f0.host", 128));
    int cnt = count_host_alignment_asserts(f, m);
    if (cnt != 3) {
        printf("Error: expected 3 host alignment assertions in code, but got %d\n", cnt);
        return -1;
    }

    printf("Success!\n");
    return 0;
}
