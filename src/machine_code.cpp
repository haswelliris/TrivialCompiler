#include "machine_code.hpp"

#include <iomanip>

std::ostream &operator<<(std::ostream &os, const MachineProgram &p) {
  using std::endl;
  static const std::string BB_PREFIX = "_BB";
  IndexMapper<MachineBB> bb_index;

  // code section
  os << ".section .text" << endl;
  for (auto f = p.func.head; f; f = f->next) {
    // generate symbol for function
    os << endl << ".global " << f->func->func->name << endl;
    os << "\t"
       << ".type"
       << "\t" << f->func->func->name << ", %function" << endl;
    os << f->func->func->name << ":" << endl;

    // function prologue
    os << "\tstmfd\tsp!, {r4-r11,lr}" << endl;
    // fp = sp
    os << "\tmov\tr11, sp" << endl;
    // decrease sp
    os << "\tadd\tsp, sp, #-" << f->sp_offset << endl;

    auto pb = [&](MachineBB *bb) {
      os << BB_PREFIX << bb_index.get(bb);
      return "";
    };
    // generate code for each BB
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      os << pb(bb) << ":" << endl;
      os << "@ pred:";
      for (auto &pred : bb->pred) {
        os << " " << pb(pred);
      }
      os << ", succ:";
      for (auto &succ : bb->succ) {
        if (succ) {
          os << " " << pb(succ);
        }
      }
      os << ", livein:";
      for (auto &op : bb->livein) {
        os << " " << op;
      }
      os << ", liveout:";
      for (auto &op : bb->liveout) {
        os << " " << op;
      }
      os << ", liveuse:";
      for (auto &use : bb->liveuse) {
        os << " " << use;
      }
      os << ", def:";
      for (auto &def : bb->def) {
        os << " " << def;
      }
      os << endl;

      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (inst == bb->control_transfer_inst) {
          os << "@ control transfer" << endl;
        }
        os << "\t";
        if (auto x = dyn_cast<MIJump>(inst)) {
          os << "b"
             << "\t" << pb(x->target) << endl;
        } else if (auto x = dyn_cast<MIBranch>(inst)) {
          os << "b" << x->cond << "\t" << pb(x->target) << endl;
        } else if (auto x = dyn_cast<MIAccess>(inst)) {
          MachineOperand data{};
          std::string inst_name;
          if (auto x = dyn_cast<MILoad>(inst)) {
            data = x->dst;
            inst_name = "ldr";
          } else if (auto x = dyn_cast<MIStore>(inst)) {
            data = x->data;
            inst_name = "str";
          }
          if (x->offset.state == MachineOperand::Immediate) {
            i32 offset = x->offset.value << x->shift;
            os << inst_name << "\t" << data << ", [" << x->addr << ", #" << offset << "]" << endl;
          } else {
            os << inst_name << "\t" << data << ", [" << x->addr << ", " << x->offset << ", LSL #" << x->shift << "]"
               << endl;
          }
        } else if (auto x = dyn_cast<MIGlobal>(inst)) {
          os << "ldr"
             << "\t" << x->dst << ", =" << x->sym->name << endl;
        } else if (auto x = dyn_cast<MIBinary>(inst)) {
          const char *op = "unknown";
          if (x->tag == MachineInst::Mul) {
            op = "mul";
            if (x->dst == x->lhs) {
              dbg("Rd and Rm must be different in MUL instruction, swapping Rm and Rn...");
              if (x->dst == x->rhs) {
                ERR_EXIT(CODEGEN_ERROR, "Rm is same with Rn, could not help");
              }
              std::swap(x->lhs, x->rhs);
            }
          } else if (x->tag == MachineInst::Add) {
            op = "add";
          } else if (x->tag == MachineInst::Sub) {
            op = "sub";
          } else if (x->tag == MachineInst::Mod) {
            op = "mod";  // TODO: no MOD instruction here
          } else if (x->tag == MachineInst::And) {
            op = "and";
          } else if (x->tag == MachineInst::Or) {
            op = "orr";
          } else {
            UNREACHABLE();
          }
          os << op << "\t" << x->dst << ", " << x->lhs << ", " << x->rhs << endl;
        } else if (auto x = dyn_cast<MIUnary>(inst)) {
          UNREACHABLE();
        } else if (auto x = dyn_cast<MICompare>(inst)) {
          os << "cmp"
             << "\t" << x->lhs << ", " << x->rhs << endl;
        } else if (auto x = dyn_cast<MIMove>(inst)) {
          // limit of ARM immediate number, see
          // https://stackoverflow.com/questions/10261300/invalid-constant-after-fixup
          if (x->rhs.state == MachineOperand::Immediate && !can_encode_imm(x->rhs.value)) {
            dbg("Immediate number in MIMove split to higher and lower bits");
            // split into high & low 16 bits
            u32 imm = x->rhs.value;
            auto low_operand = MachineOperand{.state = MachineOperand::Immediate, .value = (i32)(imm & 0xffffu)};
            os << "movw"
               << "\t" << x->dst << ", " << low_operand << endl;
            if (imm > 0xffffu) {
              auto high_operand = MachineOperand{.state = MachineOperand::Immediate, .value = (i32)(imm >> 16u)};
              os << "\tmovt"
                 << "\t" << x->dst << ", " << high_operand << endl;
            }
          } else {
            os << "mov" << x->cond << "\t" << x->dst << ", " << x->rhs << endl;
          }
        } else if (auto x = dyn_cast<MIReturn>(inst)) {
          // function epilogue
          // restore registers and pc from stack
          // increase sp
          os << "add\tsp, sp, #" << f->sp_offset << endl;
          os << "\tldmfd\t sp!, {r4-r11,pc}" << endl;
        } else if (auto x = dyn_cast<MICall>(inst)) {
          os << "blx\t" << x->func->name << endl;
        } else if (auto x = dyn_cast<MIComment>(inst)) {
          os << "@ " << x->content << endl;
        } else {
          UNREACHABLE();
        }
      }
    }
  }
  // data section
  os << endl << endl << ".section .data" << endl;
  os << ".align 4" << endl;
  for (auto &decl : p.glob_decl) {
    os << endl << ".global " << decl->name << endl;
    os << "\t"
       << ".type"
       << "\t" << decl->name << ", %object" << endl;
    os << decl->name << ":" << endl;

    // output values
    int count = 0;
    bool initialized = false;
    i32 last = 0;

    auto print_values = [&]() {
      using std::hex;
      using std::dec;
      // TODO: print in hex?
      if (count > 1) {
        os << "\t"
           << ".fill"
           << "\t" << count << ", 4, " << last << endl;
      } else {
        os << "\t"
           << ".long"
           << "\t" << last << endl;
      }
    };

    for (auto expr : decl->flatten_init) {
      if (!initialized) {
        initialized = true;
        last = expr->result;
      }
      if (expr->result == last) {
        ++count;
      } else {
        print_values();
        last = expr->result;
        count = 1;
      }
    }
    print_values();
  }
  return os;
}
