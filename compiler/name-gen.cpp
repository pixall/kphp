#include "compiler/name-gen.h"

#include "compiler/compiler-core.h"
#include "compiler/const-manipulations.h"
#include "compiler/data/class-data.h"
#include "compiler/data/src-file.h"
#include "compiler/gentree.h"
#include "compiler/io.h"
#include "compiler/pipes/register-variables.h"
#include "compiler/stage.h"

string register_unique_name(const string &prefix) {
  //static set <string> v;
  //static volatile int x = 0;
  //AutoLocker <volatile int *> locker (&x);
  //if (v.count (prefix)) {
  //fprintf (stderr, "%s\n", prefix.c_str());
  //assert (0);
  //}
  //v.insert (prefix);
  return prefix;
}

static inline string gen_unique_name_inside_file(const std::string &prefix, volatile int &x, map<unsigned long long, int> &name_map) {
  AUTO_PROF (next_name);
  AutoLocker<volatile int *> locker(&x);
  SrcFilePtr file = stage::get_file();
  unsigned long long h = hash_ll(file->unified_file_name);
  int *i = &(name_map[h]);
  int cur_i = (*i)++;
  char tmp[50];
  sprintf(tmp, "%llx_%d", h, cur_i);

  return prefix + "$ut" + tmp;
}

string gen_shorthand_ternary_name() {
  static volatile int x = 0;
  static map<unsigned long long, int> name_map;

  return gen_unique_name_inside_file("shorthand_ternary_cond", x, name_map);
}

string gen_anonymous_function_name() {
  static volatile int x = 0;
  static map<unsigned long long, int> name_map;

  return gen_unique_name_inside_file("anon", x, name_map);
}


string gen_const_string_name(const string &str) {
  AUTO_PROF (next_const_string_name);
  unsigned long long h = hash_ll(str);
  char tmp[50];
  sprintf(tmp, "const_string$us%llx", h);
  return tmp;
}

string gen_const_regexp_name(const string &str) {
  AUTO_PROF (next_const_string_name);
  unsigned long long h = hash_ll(str);
  char tmp[50];
  sprintf(tmp, "const_regexp$us%llx", h);
  return tmp;
}

bool is_array_suitable_for_hashing(VertexPtr vertex) {
  return vertex->type() == op_array && CheckConst::is_const(vertex);
}

string gen_const_array_name(const VertexAdaptor<op_array> &array) {
  char tmp[50];
  sprintf(tmp, "const_array$us%llx", ArrayHash::calc_hash(array));
  return tmp;
}

string gen_unique_name(string prefix, bool flag) {
  for (int i = 0; i < (int)prefix.size(); i++) {
    int c = prefix[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
    } else {
      prefix[i] = '_';
    }
  }
  prefix += "$u";
  AUTO_PROF (next_name);
  FunctionPtr function = stage::get_function();
  if (!function || flag) {
    return register_unique_name(prefix);
  }
  map<long long, int> *name_gen_map = &function->name_gen_map;
  int h = hash(function->name);
  long long ph = hash_ll(prefix);
  int *i = &(*name_gen_map)[ph];
  int cur_i = (*i)++;
  char tmp[50];
  sprintf(tmp, "%x_%d", h, cur_i);
  return register_unique_name(prefix + tmp);
}

inline string resolve_uses(FunctionPtr current_function, string class_name, char delim) {
  if (class_name[0] != '\\') {
    if (class_name == "static" || class_name == "self" || class_name == "parent") {
      if (class_name == "parent") {
        ClassPtr class_id = current_function->get_this_or_topmost_if_lambda()->class_id;
        // тут (пока что?) именно extends как строка, т.к. parent_class присваивается позже, чем может вызываться resolve_uses()
        auto extends_it = std::find_if(class_id->str_dependents.begin(), class_id->str_dependents.end(),
                                       [](ClassData::StrDependence &dep) { return dep.type == ctype_class; });
        kphp_assert(extends_it != class_id->str_dependents.end());
        class_name = resolve_uses(current_function, extends_it->class_name, delim);
      } else if (class_name == "static") {
        class_name = current_function->get_this_or_topmost_if_lambda()->context_class->name;
      } else {
        class_name = current_function->get_this_or_topmost_if_lambda()->class_id->name;
      }
    } else {
      size_t slash_pos = class_name.find('\\');
      if (slash_pos == string::npos) {
        slash_pos = class_name.length();
      }
      string class_name_start = class_name.substr(0, slash_pos);
      auto uses_it = current_function->file_id->namespace_uses.find(class_name_start);

      if (uses_it != current_function->file_id->namespace_uses.end()) {
        class_name = uses_it->second + class_name.substr(class_name_start.length());
      } else {
        class_name = current_function->file_id->namespace_name + "\\" + class_name;
      }
    }
  }
  if (class_name[0] == '\\') {
    class_name = class_name.substr(1);
  }
  if (delim != '\\') {
    std::replace(class_name.begin(), class_name.end(), '\\', delim);
  }

  return class_name;
}

static const char *_err_instance_access(VertexPtr v, const char *desc) {
  if (v->type() == op_func_call) {
    return strdup(std::string("Invalid call ...->" + v->get_string() + "(): " + desc).c_str());
  }

  return strdup(std::string("Invalid property ...->" + v->get_string() + ": " + desc).c_str());
}

/*
 * Если 'new A(...)', то на самом деле это вызов A$$__construct(...), если не special case.
 */
string resolve_constructor_func_name(FunctionPtr function, VertexAdaptor<op_constructor_call> ctor_call) {
  if (likely(!ctor_call->type_help)) {
    return resolve_uses(function, ctor_call->get_string()) + "$$" + "__construct";
  }

  return "new_" + ctor_call->get_string();   // Memcache, RpcMemcache, Exception, true_mc, test_mc, rich_mc
}

/*
 * На уровне gentree конструкция '...->method(...)' превращается в 'SOMEMETHOD(...,...)'.
 * Вот тут определяем, что за SOMEMETHOD — это из какого-то класса — именно того, что в левой части (= первый параметр).
 * Например, $a->method(), если $a имеет тип Classes\A, то на самом деле это Classes$A$$method
 */
string resolve_instance_func_name(FunctionPtr function, VertexAdaptor<op_func_call> arrow_call) {
  ClassPtr klass = resolve_class_of_arrow_access(function, arrow_call);

  if (likely(klass && klass->new_function)) {
    return replace_backslashes(klass->name) + "$$" + arrow_call->get_string();
  }

  // особый кейс зарезервированных классов: $mc->get() это memcached_get($mc) и пр.
  if (klass && klass->name == "Exception") {
    return "exception_" + arrow_call->get_string();
  }
  if (klass && klass->name == "Memcache") {
    return "memcached_" + arrow_call->get_string();
  }

  return std::string();
}

/*
 * Когда есть любое выражение lhs перед стрелочкой ('$a->...', '(new A())->...', 'get()->nestedArr[0]->...'),
 * то слева ожидается инстанс какого-то класса.
 * Определяем, что это за класс, и кидаем осмысленную ошибку, если там оказалось что-то не то.
 * Например, '$a = 42; $a->...' скажет, что '$a is not an instance'
 */
ClassPtr resolve_class_of_arrow_access(FunctionPtr function, VertexPtr v) {
  // тут всего 2 варианта типа v:
  // 1) lhs->f(...args), что заменилось на f(lhs,...args)
  // 2) lhs->propname
  kphp_assert((v->type() == op_func_call && v->extra_type == op_ex_func_member) || v->type() == op_instance_prop);

  VertexPtr lhs = v->type() == op_instance_prop ?
                  v.as<op_instance_prop>()->instance() :
                  v.as<op_func_call>()->args()[0];
  ClassPtr klass;

  switch (lhs->type()) {
    // (new A)->...
    case op_constructor_call: {
      AssumType assum = infer_class_of_expr(function, lhs, klass);
      kphp_assert(assum == assum_instance && klass);
      return klass;
    }

      // $var->...
    case op_var: {
      AssumType assum = infer_class_of_expr(function, lhs, klass);
      kphp_error(assum == assum_instance,
                 _err_instance_access(v, format("$%s is not an instance", lhs->get_c_string())));
      return klass;
    }

      // getInstance()->...
    case op_func_call: {
      AssumType assum = infer_class_of_expr(function, lhs, klass);
      kphp_error(assum == assum_instance,
                 _err_instance_access(v, format("%s() does not return instance", lhs->get_c_string())));
      return klass;
    }

      // ...->anotherInstance->...
    case op_instance_prop: {
      AssumType assum = infer_class_of_expr(function, lhs, klass);
      kphp_error(assum == assum_instance,
                 _err_instance_access(v, format("$%s->%s is not an instance", lhs.as<op_instance_prop>()->instance()->get_c_string(), lhs->get_c_string())));
      return klass;
    }

      // ...[$idx]->...
    case op_index: {
      VertexAdaptor<op_index> index = lhs.as<op_index>();
      VertexPtr array = index->array();
      if (index->has_key()) {
        // $var[$idx]->...
        if (array->type() == op_var) {
          AssumType assum = infer_class_of_expr(function, array, klass);
          kphp_error(assum == assum_instance_array,
                     _err_instance_access(v, format("$%s is not an array of instances", array->get_c_string())));
          return klass;
        }
        // getArr()[$idx]->...
        if (array->type() == op_func_call) {
          AssumType assum = infer_class_of_expr(function, array, klass);
          kphp_error(assum == assum_instance_array,
                     _err_instance_access(v, format("%s() does not return array of instances", array->get_c_string())));
          return klass;
        }
        // ...->arrOfInstances[$idx]->...
        if (array->type() == op_instance_prop) {
          AssumType assum = infer_class_of_expr(function, array, klass);
          kphp_error(assum == assum_instance_array,
                     _err_instance_access(v, format("$%s->%s is not array of instances",
                                                     array.as<op_instance_prop>()->instance()->get_c_string(), array->get_c_string())));
          return klass;
        }
      }
    }

      /* fallthrough */
    default:
      kphp_error(false, _err_instance_access(v, "Can not parse: maybe, too deep nesting"));
      return klass;
  }
}


string get_context_by_prefix(FunctionPtr function, const string &class_name, char delim) {
  if (class_name == "static" || class_name == "self" || class_name == "parent") {
    return resolve_uses(function, "\\" + function->get_this_or_topmost_if_lambda()->context_class->name, delim);
  }
  return resolve_uses(function, class_name, delim);
}

string get_full_static_member_name(FunctionPtr function, const string &name, bool append_with_context) {
  size_t pos$$ = name.find("::");

  if (pos$$ == string::npos) {
    return name;
  }

  string prefix_name = name.substr(0, pos$$);
  string class_name = resolve_uses(function, prefix_name, '$');
  string fun_name = name.substr(pos$$ + 2);
  string new_name = class_name + "$$" + fun_name;
  if (!function->is_lambda() && append_with_context) {
    string context_name = get_context_by_prefix(function, prefix_name);
    if (context_name != class_name) {
      new_name += "$$" + context_name;
    }
  }
  return new_name;
}

string resolve_define_name(string name) {
  size_t pos$$ = name.find("$$");
  if (pos$$ != string::npos) {
    string class_name = name.substr(0, pos$$);
    string define_name = name.substr(pos$$ + 2);
    const string &real_class_name = replace_characters(class_name, '$', '\\');
    ClassPtr klass = G->get_class(real_class_name);
    if (klass) {
      while (klass && !klass->members.has_constant(define_name)) {
        klass = klass->parent_class;
      }
      if (klass) {
        name = "c#" + replace_backslashes(klass->name) + "$$" + define_name;
      }
    }
  }
  return name;
}
