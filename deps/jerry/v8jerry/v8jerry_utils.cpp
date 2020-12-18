#include "v8jerry_utils.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "v8jerry_atomics.hpp"
#include "v8jerry_flags.hpp"
#include "v8jerry_value.hpp"

#define DEBUG_PRINT 1

JerryPolyfill::JerryPolyfill(const char* name, const char* fn_args, const char* fn_body)
    : m_method(JerryPolyfill::BuildMethod(name, fn_args, fn_body))
{
}

JerryPolyfill::~JerryPolyfill() {
    jerry_release_value(m_method);
}

jerry_value_t JerryPolyfill::Call(const jerry_value_t this_arg, const jerry_value_t *args, int argc) const {
    jerry_value_t result = jerry_call_function(m_method, this_arg, args, argc);
    return result;
}

jerry_value_t JerryPolyfill::BuildMethod(const char* name, const char* fn_args, const char* fn_body) {
    jerry_value_t method = jerry_parse_function(reinterpret_cast<const jerry_char_t*>(name), strlen(name),
                                 reinterpret_cast<const jerry_char_t*>(fn_args), strlen(fn_args),
                                 reinterpret_cast<const jerry_char_t*>(fn_body), strlen(fn_body),
                                 JERRY_PARSE_NO_OPTS);
    if (jerry_value_is_error(method)) {
        fprintf(stderr, "Failed to build helper method initialize at: %s:%d\nfunction (%s) {\n%s\n}", __FILE__, __LINE__, fn_args, fn_body);
        abort();
    }

    return method;
}

#if DEBUG_PRINT

static jerry_value_t
jerryx_handler_print (const jerry_value_t func_obj_val,
                      const jerry_value_t this_p,
                      const jerry_value_t args_p[],
                      const jerry_length_t args_cnt) {
  (void) func_obj_val; /* unused */
  (void) this_p; /* unused */

  const char * const null_str = "\\u0000";

  jerry_value_t ret_val = jerry_create_undefined ();

  for (jerry_length_t arg_index = 0; arg_index < args_cnt; arg_index++)
  {
    jerry_value_t str_val;

    if (jerry_value_is_symbol (args_p[arg_index]))
    {
      str_val = jerry_get_symbol_descriptive_string (args_p[arg_index]);
    }
    else
    {
      str_val = jerry_value_to_string (args_p[arg_index]);
    }

    if (jerry_value_is_error (str_val))
    {
      /* There is no need to free the undefined value. */
      ret_val = str_val;
      break;
    }

    jerry_length_t length = jerry_get_utf8_string_length (str_val);
    jerry_length_t substr_pos = 0;
    jerry_char_t substr_buf[256];

    do
    {
      jerry_size_t substr_size = jerry_substring_to_utf8_char_buffer (str_val,
                                                                      substr_pos,
                                                                      length,
                                                                      substr_buf,
                                                                      256 - 1);

      jerry_char_t *buf_end_p = substr_buf + substr_size;

      /* Update start position by the number of utf-8 characters. */
      for (jerry_char_t *buf_p = substr_buf; buf_p < buf_end_p; buf_p++)
      {
        /* Skip intermediate utf-8 octets. */
        if ((*buf_p & 0xc0) != 0x80)
        {
          substr_pos++;
        }
      }

      if (substr_pos == length)
      {
        *buf_end_p++ = (arg_index < args_cnt - 1) ? ' ' : '\n';
      }

      for (jerry_char_t *buf_p = substr_buf; buf_p < buf_end_p; buf_p++)
      {
        char chr = (char) *buf_p;

        if (chr != '\0')
        {
          jerry_port_print_char (chr);
          continue;
        }

        for (jerry_size_t null_index = 0; null_str[null_index] != '\0'; null_index++)
        {
          jerry_port_print_char (null_str[null_index]);
        }
      }
    }
    while (substr_pos < length);

    jerry_release_value (str_val);
  }

  if (args_cnt == 0 || jerry_value_is_error (ret_val))
  {
    jerry_port_print_char ('\n');
  }

  return ret_val;
}

#endif

static jerry_value_t
jerryx_handler_string_normalize (const jerry_value_t func_obj_val,
                                 const jerry_value_t this_p,
                                 const jerry_value_t args_p[],
                                 const jerry_length_t args_cnt) {
  (void) func_obj_val; /* unused */
  (void) args_p; /* unused */
  (void) args_cnt; /* unused */

  return jerry_acquire_value(this_p);
}

static jerry_value_t JerryHandlerStackTrace(const jerry_value_t func,
                                            const jerry_value_t this_val,
                                            const jerry_value_t args_p[],
                                            const jerry_length_t args_count)
{
    if (!jerry_is_feature_enabled(JERRY_FEATURE_LINE_INFO) || args_count == 0 || !jerry_value_is_object(args_p[0]))
    {
        printf("Line info is disabled or the argument is not an object.\n");
        return jerry_create_undefined();
    }

    jerry_value_t stack_string = jerry_create_string((const jerry_char_t*)"stack");
    jerry_value_t stack_trace = jerry_get_backtrace(0);
    jerry_value_t set_result = jerry_set_property(args_p[0], stack_string, stack_trace);

    if (jerry_value_is_error(set_result))
    {
        printf("Cannot set stack property.\n");
    }

    jerry_release_value(stack_string);
    jerry_release_value(stack_trace);
    jerry_release_value(set_result);

    return jerry_create_undefined();
}

static jerry_value_t JerryHandlerGC(const jerry_value_t func,
                                    const jerry_value_t thisarg,
                                    const jerry_value_t argv[],
                                    const jerry_value_t argc) {
    jerry_gc (JERRY_GC_PRESSURE_LOW);
    return jerry_create_undefined();
}

void InjectGlobalFunctions(void) {
#if DEBUG_PRINT
    JerryxHandlerRegisterGlobal((const jerry_char_t *)"print", jerryx_handler_print);
#endif
    JerryxHandlerRegisterString((const jerry_char_t *)"normalize", jerryx_handler_string_normalize);

    JerryAtomics::Initialize();

    JerryValue global(jerry_get_global_object());

    if (Flag::Get(Flag::expose_gc)->u.bool_value) {
        JerryValue gc_string(jerry_create_string((const jerry_char_t*)"gc"));
        JerryValue gc_function(jerry_create_external_function(JerryHandlerGC));

        global.SetProperty(&gc_string, &gc_function);
    }

    JerryValue error_string(jerry_create_string((const jerry_char_t*)"Error"));
    JerryValue capture_stack_trace_string(jerry_create_string((const jerry_char_t*)"captureStackTrace"));
    JerryValue stack_trace_function(jerry_create_external_function(JerryHandlerStackTrace));

    JerryValue* error_obj = global.GetProperty(&error_string);
    if (error_obj == NULL) {
        printf("Error object is not defined...\n");
        abort();
    }

    error_obj->SetProperty(&capture_stack_trace_string, &stack_trace_function);
    delete error_obj;
}

// TODO: remove these layering violations (this is a Jerry internal method, should not be visible here)
extern "C" bool ecma_get_object_is_builtin(void* obj);
#define ECMA_OBJECT_REF_ONE (1u << 6)
#define ECMA_OBJECT_MAX_REF (0x3ffu << 6)
#define ECMA_VALUE_TYPE_MASK 0x7u
#define ECMA_VALUE_SHIFT 3
typedef uint32_t ecma_value_t;

extern "C" void * jmem_decompress_pointer (uintptr_t compressed_pointer);

typedef struct {
  /** type : 4 bit : ecma_object_type_t or ecma_lexical_environment_type_t
                     depending on ECMA_OBJECT_FLAG_BUILT_IN_OR_LEXICAL_ENV
      flags : 2 bit : ECMA_OBJECT_FLAG_BUILT_IN_OR_LEXICAL_ENV,
                      ECMA_OBJECT_FLAG_EXTENSIBLE or ECMA_OBJECT_FLAG_NON_CLOSURE
      refs : 10 bit (max 1023) */
  uint16_t type_flags_refs;
  /* snippet from JerryScript code */
} header_ecma_object_t;

static void *ecma_get_pointer_from_ecma_value (ecma_value_t value) {
#if __UINTPTR_MAX__ <= __UINT32_MAX__
  void *ptr = (void *) (uintptr_t) ((value) & ~ECMA_VALUE_TYPE_MASK);
#else
  void *ptr = (void*)jmem_decompress_pointer(value >> ECMA_VALUE_SHIFT);
#endif
  return ptr;
}

static bool collect_objects(const jerry_value_t object, void *user_data_p) {
    std::vector<jerry_value_t>* objects = reinterpret_cast<std::vector<jerry_value_t>*>(user_data_p);

    objects->push_back(object);

    return true;
}

static bool ecam_have_ref(const jerry_value_t value) {
    header_ecma_object_t* header = reinterpret_cast<header_ecma_object_t*>(ecma_get_pointer_from_ecma_value(value));
    return header->type_flags_refs >= ECMA_OBJECT_REF_ONE;
}

void JerryForceCleanup(void) {
    jerry_gc(JERRY_GC_PRESSURE_HIGH);

    std::vector<jerry_value_t> objects;
    jerry_objects_foreach(collect_objects, &objects);

    for (size_t idx = 0; idx < objects.size(); idx++) {
        bool is_builtin = ecma_get_object_is_builtin(ecma_get_pointer_from_ecma_value(objects[idx]));
        if (!is_builtin && ecam_have_ref(objects[idx])) {
            jerry_release_value(objects[idx]);
        }
    }
}

void JerryxHandlerRegister (const jerry_char_t *name_p, jerry_value_t object_value,
                            jerry_external_handler_t handler_p) {
    jerry_value_t function_name_val = jerry_create_string (name_p);
    jerry_value_t function_val = jerry_create_external_function (handler_p);
    jerry_property_descriptor_t desc;
    jerry_init_property_descriptor_fields(&desc);
    desc.is_value_defined = true;
    desc.value = function_val;

    jerry_value_t result_val = jerry_define_own_property (object_value, function_name_val, &desc);

    jerry_free_property_descriptor_fields (&desc);
    jerry_release_value (function_name_val);

    if (jerry_value_is_error (result_val))
    {
        jerry_port_log (JERRY_LOG_LEVEL_WARNING, "Warning: failed to register '%s' method.", name_p);
    }

    jerry_release_value (result_val);
}

void JerryxHandlerRegisterGlobal (const jerry_char_t *name_p,
                                  jerry_external_handler_t handler_p) {
    jerry_value_t global_obj_val = jerry_get_global_object();
    JerryxHandlerRegister(name_p, global_obj_val, handler_p);
    jerry_release_value(global_obj_val);
}

void JerryxHandlerRegisterString (const jerry_char_t *name_p,
                                  jerry_external_handler_t handler_p) {
    jerry_value_t global_obj_val = jerry_get_global_object();
    jerry_value_t name_val = jerry_create_string((const jerry_char_t *) "String");
    jerry_value_t string_val = jerry_get_property(global_obj_val, name_val);
    jerry_release_value (name_val);
    jerry_release_value (global_obj_val);

    name_val = jerry_create_string((const jerry_char_t *) "prototype");
    jerry_value_t prototype_val = jerry_get_property(string_val, name_val);
    jerry_release_value (name_val);
    jerry_release_value (string_val);

    JerryxHandlerRegister (name_p, prototype_val, handler_p);
    jerry_release_value (prototype_val);
}
