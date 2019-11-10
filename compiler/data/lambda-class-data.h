#pragma once

#include "compiler/data/class-data.h"
#include "compiler/data/data_ptr.h"

class LambdaClassData final : public ClassData {
public:
  LambdaClassData() = default;

  static LambdaPtr get_from(VertexPtr v);

  void infer_uses_assumptions(FunctionPtr parent_function);

  FunctionPtr get_template_of_invoke_function() const;

  std::string get_name_of_invoke_function_for_extern(VertexAdaptor<op_func_call> extern_function_call,
                                                     FunctionPtr function_context,
                                                     std::map<int, Assumption> *template_type_id_to_ClassPtr = nullptr,
                                                     FunctionPtr *template_of_invoke_method = nullptr) const;

  VertexPtr gen_constructor_call_pass_fields_as_args() const;
  VertexAdaptor<op_constructor_call> gen_constructor_call_with_args(std::vector<VertexPtr> args) const;

  const char *get_name() const final {
    return is_static ? "static lambda" : "lambda";
  }

  static const std::string &get_lambda_namespace() {
    static std::string lambda_namespace("L");
    return lambda_namespace;
  }

  static const char *get_parent_this_name() {
    return "parent$this";
  }

  std::string get_namespace() const final {
    return get_lambda_namespace();
  }

  bool is_lambda() const final {
    return true;
  }

  std::string get_subdir() const final {
    return "cl_l";
  }

public:
  bool is_static = false;
};
