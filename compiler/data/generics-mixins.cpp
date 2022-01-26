// Compiler for PHP (aka KPHP)
// Copyright (c) 2021 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "compiler/data/generics-mixins.h"
#include "compiler/data/function-data.h"
#include "compiler/lexer.h"
#include "compiler/phpdoc.h"
#include "compiler/type-hint.h"
#include "compiler/vertex.h"
#include "compiler/utils/string-utils.h"

/*
 * How do template functions work.
 * todo reconsider comment
 * 
 * "template functions" are functions marked with @kphp-template, which expresses `f<T>` with phpdoc at declaration.
 * Unlike regular functions, they can be called with various instances: `f($a)`, `f($b)`, etc., implicitly meaning `f<A>($a)` and so on.
 * `f($primitive)` is proxied to `f<any>`.
 *
 * When `f<T>` is called like `f($obj)`, we detect T by an assumption (say, SomeClass) and create a function named "f$_$SomeClass".
 * `f($a)` makes T=A, `f($arr_or_users)` makes T=User[].
 * We can't express `f<User>($arr_of_users)` now, we don't have a syntax yet.
 *
 * Later, template functions will be deprecated in favor of generics support.
 * For now, we call them "template functions", but also use the word "generics" in some comments and sources.
 *
 * Say, we want to express `f<T1, T2>(T1 $arg0, T1 $arg1, int $arg2, T2 $arg3): T2[]`
 * 1) an old-fashioned syntax — still supported, but deprecated:
 *    @kphp-template $arg0 $arg1       // means they must be of the same classes
 *    @kphp-template T $arg3           // may be of another class; T to be used with @kphp-return
 *    @kphp-return T[]                 // for assumptions outerwise
 * 2) a new-way syntax — that looks more like generics:
 *    @kphp-template T1 T2
 *    @kphp-param T1 $arg0
 *    @kphp-param T1 $arg1
 *    @kphp-param T2 $arg3
 *    @kphp-return T2[]
 * For now, @kphp-param can be still only T1/T2 — not T1[] or other expressions, due to the reason described above.
 * Later, generics will support T1[], or tuple(T1,T2), or others — as T's will be able to be specified explicitly at a call.
 * 
 * T1 and T2 are represented as TypeHintGenericsT, to differ them from regular instances.
 * An instantiation (T=User[] for example) is performed by `phpdoc_replace_genericsT_with_instantiation()`.
 *
 * Functions with untyped `callable` arguments are auto-converted to template functions.
 * Later, we'll have a generics syntax to express `<T : SomeInterface>`, then SomeInterface would be `extends_hint` in terms of the code.
 * For now, extends_hint can only be an untyped callable, it's emerged from `f(callable $fn)`.
 * Now it's used to deduce, that `f('strlen')` is f<lambda_class>, not f<string>.
 *
 * call->instantiation_list is calculated in DeduceImplicitTypesAndCastsPass.
 * Functions are created in InstantiateGenericsAndLambdasPass.
 */


// an old syntax has only @kphp-template with $argument inside (or many such tags)
static void create_from_phpdoc_old_syntax(FunctionPtr f, GenericsDeclarationMixin *out, const PhpDocComment *phpdoc) {
  for (const PhpDocTag &tag : phpdoc->tags) {
    switch (tag.type) {
      case PhpDocType::kphp_template: {
        std::string nameT;    // can be before $arg
        const TypeHint *extends_hint = nullptr;
        for (const auto &var_name : split_skipping_delimeters(tag.value, ", ")) {
          if (var_name[0] != '$') {
            kphp_error_return(nameT.empty(), "invalid @kphp-template syntax");
            nameT = static_cast<std::string>(var_name);
            continue;
          }
          if (nameT.empty()) {
            nameT = "T" + std::to_string(out->size() + 1);
          }

          auto param = f->find_param_by_name(var_name.substr(1));
          kphp_error_return(param, fmt_format("@kphp-template for {} — argument not found", var_name));
          extends_hint = param->type_hint && param->type_hint->try_as<TypeHintCallable>() ? param->type_hint : nullptr;
          param->type_hint = TypeHintGenericsT::create(nameT);
        }

        kphp_error_return(!nameT.empty(), "invalid @kphp-template syntax");
        out->add_itemT(nameT, extends_hint ?: TypeHintPrimitive::create(tp_any));
        break;
      }

      case PhpDocType::kphp_param:
        kphp_error(0, "@kphp-param is not acceptable with old-style @kphp-template syntax");
        break;

      case PhpDocType::kphp_return: {
        kphp_error_return(!out->empty(), "@kphp-template must precede @kphp-return");
        if (auto tag_parsed = tag.value_as_type_and_var_name(f)) {
          f->return_typehint = tag_parsed.type_hint;    // typically, T or T::field
        }
        break;
      }

      default:
        break;
    }
  }

  kphp_assert(!f->generics_declaration->empty());
}

// a new syntax is "@kphp-template T1, T2" with many @kphp-param tags describing params depending on T's
// every T may have a where-condition: "T: callable" / "T: ClassName"
static void create_from_phpdoc_new_syntax(FunctionPtr f, GenericsDeclarationMixin *out, const PhpDocComment *phpdoc) {
  for (const PhpDocTag &tag : phpdoc->tags) {
    switch (tag.type) {
      case PhpDocType::kphp_template:
        for (auto s : split_skipping_delimeters(tag.value, ",")) {
          vk::string_view v = vk::trim(s);
          size_t pos_colon = v.find(':');

          if (pos_colon == std::string::npos) {
            // just "T" / "T1" / etc.
            std::string nameT = static_cast<std::string>(v);
            out->add_itemT(nameT, TypeHintPrimitive::create(tp_any));

          } else {
            // "T:callable" / "T:ClassName" / "T:SomeInterface"
            std::string nameT = static_cast<std::string>(vk::trim(v.substr(0, pos_colon)));
            vk::string_view extends_str = vk::trim(v.substr(pos_colon + 1));
            const TypeHint *extends_hint = nullptr;

            // todo support ClassName, support more options, maybe use real tokenizer
            if (extends_str == "callable") {
              extends_hint = TypeHintCallable::create_untyped_callable();
            } else {
              kphp_error(0, fmt_format("Invalid generics declaration syntax after '{}:'", nameT));
              extends_hint = TypeHintPrimitive::create(tp_any);
            }

            out->add_itemT(nameT, extends_hint);
          }
        }
        break;

      case PhpDocType::kphp_param: {
        kphp_error_return(!out->empty(), "@kphp-template must precede @kphp-param");
        if (auto tag_parsed = tag.value_as_type_and_var_name(f)) {
          auto param = f->find_param_by_name(tag_parsed.var_name);
          kphp_error_return(param, fmt_format("@kphp-param for unexisting argument ${}", tag_parsed.var_name));
          param->type_hint = tag_parsed.type_hint;
        }
        break;
      }

      case PhpDocType::kphp_return:
        kphp_error_return(!out->empty(), "@kphp-template must precede @kphp-return");
        if (auto tag_parsed = tag.value_as_type_and_var_name(f)) {
          f->return_typehint = tag_parsed.type_hint;    // typically, T or T::field
        }
        break;

      default:
        break;
    }
  }

  kphp_assert(!f->generics_declaration->empty());
}


bool GenericsDeclarationMixin::has_nameT(const std::string &nameT) const {
  for (const GenericsItem &item : itemsT) {
    if (item.nameT == nameT) {
      return true;
    }
  }
  return false;
}

void GenericsDeclarationMixin::add_itemT(const std::string &nameT, const TypeHint *extends_hint) {
  kphp_error_return(!nameT.empty(), "Invalid (empty) generics <T> in declaration");
  kphp_error_return(!find(nameT), fmt_format("Duplicate generics <{}> in declaration", nameT));
  itemsT.emplace_back(GenericsItem{nameT, extends_hint});
}

const TypeHint *GenericsDeclarationMixin::find(const std::string &nameT) const {
  for (const GenericsItem &item : itemsT) {
    if (item.nameT == nameT) {
      return item.extends_hint;
    }
  }
  return nullptr;
}

std::string GenericsDeclarationMixin::prompt_provide_types_human_readable(VertexPtr call) const {
  std::string call_str = replace_characters(call->get_string(), '$', ':');
  std::string params_str = call->size() > (call->extra_type == op_ex_func_call_arrow ? 1 : 0) ? "(...)" : "()";
  std::string t_str = vk::join(itemsT, ", ", [](const GenericsItem &itemT) { return itemT.nameT; });
  return "Please, provide all generics types using syntax " + call_str + "/*<" + t_str + ">*/" + params_str;
}


void GenericsInstantiationPhpComment::parse_php_comment(FunctionPtr current_function) {
  PhpDocTypeHintParser parser(current_function);
  std::vector<Token> tokens = phpdoc_to_tokens(raw_comment);
  auto cur_tok = tokens.cbegin();

  while (cur_tok != tokens.cend() && cur_tok->type() != tok_end) {
    const TypeHint *type_hint = nullptr;
    try {
      type_hint = parser.parse_from_tokens(cur_tok);
    } catch (std::runtime_error &ex) {
      kphp_error_return(0, fmt_format("Could not parse generics instantiation: {}", ex.what()));
    }

    kphp_error_return(cur_tok->type() == tok_comma || cur_tok->type() == tok_end, "expected ','");
    cur_tok++;
    types.emplace_back(type_hint);
  }
}


GenericsInstantiationMixin::GenericsInstantiationMixin(const GenericsInstantiationMixin &rhs) {
  if (rhs.php_inst != nullptr) {
    this->php_inst = new GenericsInstantiationPhpComment(*rhs.php_inst);
  }
}

void GenericsInstantiationMixin::apply_from_php_comment(const GenericsDeclarationMixin *generics_declaration, VertexPtr call) {
  kphp_assert(php_inst != nullptr && !php_inst->types.empty());

  for (int i = 0; i < php_inst->types.size() && i < generics_declaration->size(); ++i) {
    add_instantiationT(generics_declaration->itemsT[i].nameT, php_inst->types[i], call);
  }

  kphp_error(generics_declaration->size() == php_inst->types.size(),
             fmt_format("Mismatch generics instantiation count: waiting {}, got {}", generics_declaration->size(), php_inst->types.size()));
}

void GenericsInstantiationMixin::add_instantiationT(const std::string &nameT, const TypeHint *instantiation, VertexPtr call) {
  auto insertion_result = instantiations.emplace(nameT, instantiation);
  if (!insertion_result.second) {
    const TypeHint *previous_inst = insertion_result.first->second;
    FunctionPtr template_function = call.as<op_func_call>()->func_id;
    kphp_error(previous_inst == instantiation,
               fmt_format("Couldn't reify generics <{}> for call: it's both {} and {}.\n{}",
                          nameT, previous_inst->as_human_readable(), instantiation->as_human_readable(), template_function->generics_declaration->prompt_provide_types_human_readable(call)));
  }
}

const TypeHint *GenericsInstantiationMixin::find(const std::string &nameT) const {
  auto it = instantiations.find(nameT);
  return it == instantiations.end() ? nullptr : it->second;
}

std::string GenericsInstantiationMixin::generate_instantiated_func_name(FunctionPtr template_function) const {
  // an instantiated function name will be "{original_name}$_${postfix}", where postfix = "T1$_$T2"
  std::string name = template_function->name;
  for (const auto &name_and_type : instantiations) {
    name += "$_$";
    name += replace_non_alphanum(name_and_type.second->as_human_readable());
  }
  return name;
}


void GenericsDeclarationMixin::apply_from_phpdoc(FunctionPtr f, const PhpDocComment *phpdoc) {
  if (!f->generics_declaration) {
    f->generics_declaration = new GenericsDeclarationMixin();
  }
  stage::set_location(f->root->location);

  bool is_new_syntax = std::any_of(phpdoc->tags.begin(), phpdoc->tags.end(),
                                   [](const PhpDocTag &tag) { return tag.type == PhpDocType::kphp_param; });
  if (is_new_syntax) {
    create_from_phpdoc_new_syntax(f, f->generics_declaration, phpdoc);
  } else {
    create_from_phpdoc_old_syntax(f, f->generics_declaration, phpdoc);
  }
}

void GenericsDeclarationMixin::make_function_generics_on_callable_arg(FunctionPtr f, VertexPtr func_param) {
  if (!f->generics_declaration) {
    f->generics_declaration = new GenericsDeclarationMixin();
  }

  std::string nameT = "Callback" + std::to_string(f->generics_declaration->size() + 1);
  func_param.as<op_func_param>()->type_hint = TypeHintGenericsT::create(nameT);

  // add a <Cb1: callable> rule; the presence of 'callable' is important: imagine
  // function f(callable $cb) { ... }
  // f('some_fn');
  // even if f() is called with a string, it should be instantiated as f<callable$xxx>, not f<any> or f<string>
  const TypeHint *extends_hint_callable = TypeHintCallable::create_untyped_callable();
  f->generics_declaration->add_itemT(nameT, extends_hint_callable);
}
