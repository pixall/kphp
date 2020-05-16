#include "compiler/pipes/split-switch.h"

#include "compiler/data/src-file.h"
#include "compiler/function-pass.h"
#include "compiler/gentree.h"
#include "compiler/name-gen.h"

class SplitSwitchPass : public FunctionPassBase {
private:
  int depth;
  vector<FunctionPtr> new_functions;

  static VertexPtr fix_break_continue(VertexAdaptor<meta_op_goto> goto_op,
                                      const string &state_name, int cycle_depth) {
    int depth = -1;
    VertexPtr label = goto_op->level();
    if (label->type() == op_int_const) {
      depth = atoi(label->get_string().c_str());
    }
    if (depth != cycle_depth) {
      return goto_op;
    }

    auto minus_one = VertexAdaptor<op_int_const>::create();
    minus_one->str_val = "-1";
    auto state = VertexAdaptor<op_var>::create();
    state->str_val = state_name;
    auto expr = VertexAdaptor<op_set>::create(state, minus_one);

    //TODO: auto_return instead of return true!
    auto true_val = VertexAdaptor<op_true>::create();
    auto new_return = VertexAdaptor<op_return>::create(true_val);
    auto seq = VertexAdaptor<op_seq>::create(expr, new_return);
    return seq;
  }

  static VertexPtr prepare_switch_func(
    VertexPtr root,
    const string &state_name,
    int cycle_depth) {
    if (root->type() == op_return) {
      auto one = VertexAdaptor<op_int_const>::create();
      one->str_val = "1";
      auto state = VertexAdaptor<op_var>::create();
      state->str_val = state_name;
      auto expr = VertexAdaptor<op_set>::create(state, one);
      auto seq = VertexAdaptor<op_seq>::create(expr, root);
      return seq;
    }
    if ((root->type() == op_continue || root->type() == op_break)) {
      return fix_break_continue(root.as<meta_op_goto>(), state_name, cycle_depth);
    }

    for (auto &i : *root) {
      //TODO: hack... write proper Range
      bool is_cycle = OpInfo::type(i->type()) == cycle_op;
      i = prepare_switch_func(i, state_name, cycle_depth + is_cycle);
    }

    return root;
  }

public:
  SplitSwitchPass() :
    depth(0) {
  }

  string get_description() {
    return "Split switch";
  }

  bool check_function(FunctionPtr function) {
    return default_check_function(function) && function->type == FunctionData::func_global;
  }

  VertexPtr on_enter_vertex(VertexPtr root) {
    depth++;
    if (root->type() != op_switch) {
      return root;
    }

    auto switch_v = root.as<op_switch>();

    for (auto cs : switch_v->cases()) {
      VertexAdaptor<op_seq> seq;
      if (cs->type() == op_case) {
        seq = cs.as<op_case>()->cmd();
      } else if (cs->type() == op_default) {
        seq = cs.as<op_default>()->cmd();
      } else {
        kphp_fail();
      }

      string func_name = stage::get_function_name() + "$" + gen_unique_name("switch_func");

      auto case_state = VertexAdaptor<op_var>::create();
      case_state->ref_flag = true;
      string case_state_name = gen_unique_name("switch_case_state");
      case_state->str_val = case_state_name;

      auto case_state_3 = VertexAdaptor<op_var>::create();
      case_state_3->str_val = case_state_name;
      case_state_3->extra_type = op_ex_var_superlocal;
      auto case_state_0 = VertexAdaptor<op_var>::create();
      case_state_0->str_val = case_state_name;
      case_state_0->extra_type = op_ex_var_superlocal;
      auto case_state_1 = VertexAdaptor<op_var>::create();
      case_state_1->str_val = case_state_name;
      case_state_1->extra_type = op_ex_var_superlocal;
      auto case_state_2 = VertexAdaptor<op_var>::create();
      case_state_2->str_val = case_state_name;
      case_state_2->extra_type = op_ex_var_superlocal;

      auto case_state_param = VertexAdaptor<op_func_param>::create(case_state);
      auto func_params = VertexAdaptor<op_func_param_list>::create(case_state_param);
      auto func = VertexAdaptor<op_function>::create(func_params, seq);
      func = prepare_switch_func(func, case_state_name, 1).as<op_function>();
      GenTree::func_force_return(func, VertexAdaptor<op_null>::create());
      new_functions.push_back(FunctionData::create_function(func_name, func, FunctionData::func_switch));

      auto func_call = VertexAdaptor<op_func_call>::create(case_state_0);
      func_call->str_val = func_name;

      string case_res_name = gen_unique_name("switch_case_res");
      auto case_res = VertexAdaptor<op_var>::create();
      case_res->str_val = case_res_name;
      case_res->extra_type = op_ex_var_superlocal;
      auto case_res_copy = VertexAdaptor<op_var>::create();
      case_res_copy->str_val = case_res_name;
      case_res_copy->extra_type = op_ex_var_superlocal;
      auto run_func = VertexAdaptor<op_set>::create(case_res, func_call);


      auto zero = VertexAdaptor<op_int_const>::create();
      zero->str_val = "0";
      auto one = VertexAdaptor<op_int_const>::create();
      one->str_val = "1";
      auto minus_one = VertexAdaptor<op_int_const>::create();
      minus_one->str_val = "-1";

      auto eq_one = VertexAdaptor<op_eq2>::create(case_state_1, one);
      auto eq_minus_one = VertexAdaptor<op_eq2>::create(case_state_2, minus_one);

      auto cmd_one = VertexAdaptor<op_return>::create(case_res_copy);
      auto one_2 = VertexAdaptor<op_int_const>::create();
      one_2->str_val = "1";
      auto cmd_minus_one = VertexAdaptor<op_break>::create(one_2);

      auto init = VertexAdaptor<op_set>::create(case_state_3, zero);
      auto if_one = VertexAdaptor<op_if>::create(eq_one, GenTree::embrace(cmd_one));
      auto if_minus_one = VertexAdaptor<op_if>::create(eq_minus_one, GenTree::embrace(cmd_minus_one));

      vector<VertexPtr> new_seq_next;
      new_seq_next.push_back(init);
      new_seq_next.push_back(run_func);
      new_seq_next.push_back(if_one);
      new_seq_next.push_back(if_minus_one);

      auto new_seq = VertexAdaptor<op_seq>::create(new_seq_next);
      cs->back() = new_seq;
    }

    return root;
  }

  bool need_recursion(VertexPtr root) {
    return depth < 2 || root->type() == op_seq || root->type() == op_try;
  }

  VertexPtr on_exit_vertex(VertexPtr root) {
    depth--;
    return root;
  }

  const vector<FunctionPtr> &get_new_functions() {
    return new_functions;
  }
};

void SplitSwitchF::execute(FunctionPtr function, DataStream<FunctionPtr> &os) {
  SplitSwitchPass split_switch;
  run_function_pass(function, &split_switch);

  for (auto f : split_switch.get_new_functions()) {
    G->register_and_require_function(f, os, true);   // switch-функции всегда require
  }

  if (stage::has_error()) {
    return;
  }

  os << function;
}
