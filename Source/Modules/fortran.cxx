#include "swigmod.h"
#include "cparse.h"
#include <ctype.h>

namespace
{

const char usage[] = "\
Fotran Options (available with -fortran)\n\
     -noproxy    - Expose the low-level functional interface instead\n\
                   of generatingproxy classes\n\
     -final      - Generate 'final' statement to call C++ destructors\n\
\n";

//! Return a const char* with leading whitespace stripped
const char* lstrip(const_String_or_char_ptr s)
{
    const char* trimmed = Char(s);
    while (trimmed && isspace(*trimmed))
    {
        ++trimmed;
    }
    return trimmed;
}

//! Maximum line length
const int g_max_line_length = 128;

//! Print a comma-joined line of items to the given output.
int print_wrapped_line(String* out, Iterator it, int line_length)
{
    const char* prepend_comma = "";
    for (; it.item; it = Next(it))
    {
        line_length += 2 + Len(it.item);
        if (line_length >= g_max_line_length)
        {
            Printv(out, prepend_comma, NULL);
            prepend_comma = "&\n    ";
            line_length = 4;
        }
        Printv(out, prepend_comma, it.item, NULL);
        prepend_comma = ", ";
    }
    return line_length;
}

const char fortran_end_statement[] = "\n";

Wrapper* NewFortranWrapper()
{
    Wrapper* w = NewWrapper();
    w->end_statement = fortran_end_statement;
    return w;
}

}

class FORTRAN : public Language
{
  private:
    // >>> ATTRIBUTES AND OPTIONS

    String* d_module; //!< Module name
    String* d_outpath; //!< WRAP.cxx output

    bool d_use_proxy; //!< Whether to generate proxy classes
    bool d_use_final; //!< Whether to use the 'final' keyword for destructors

    // >>> OUTPUT FILES

    // Injected into .cxx file
    String* f_begin; //!< Very beginning of output file
    String* f_runtime; //!< SWIG runtime code
    String* f_header; //!< Declarations and inclusions from .i
    String* f_wrapper; //!< C++ Wrapper code
    String* f_init; //!< C++ initalization functions

    // Injected into module file
    String* f_imports;     //!< Fortran "use" directives generated from %import
    String* f_public;      //!< List of public interface functions and mapping
    String* f_types;       //!< Generated class types
    String* f_interfaces;  //!< Fortran interface declarations to SWIG functions
    String* f_proxy;    //!< Fortran subroutine wrapper functions

    // Temporary mappings
    Hash* d_overloads; //!< Overloaded subroutine -> overload names

    // Current class parameters
    Hash* d_method_overloads; //!< Overloaded subroutine -> overload names
    SwigType* d_classtype; //!< Class type

    List* d_enumvalues; //!< List of enumerator values

  public:
    virtual void main(int argc, char *argv[]);
    virtual int top(Node *n);
    virtual int functionWrapper(Node *n);
    virtual int destructorHandler(Node *n);
    virtual int constructorHandler(Node *n);
    virtual int classHandler(Node *n);
    virtual int memberfunctionHandler(Node *n);
    virtual int importDirective(Node *n);
    virtual int insertDirective(Node *n);
    virtual int enumDeclaration(Node *n);
    virtual int enumvalueDeclaration(Node *n);


    virtual String *makeParameterName(Node *n, Parm *p, int arg_num,
                                      bool is_setter = false) const;
    virtual void replaceSpecialVariables(String *method, String *tm, Parm *parm);

    FORTRAN()
        : d_module(NULL)
        , d_outpath(NULL)
        , d_use_proxy(true)
        , d_use_final(false)
        , d_classtype(NULL)
        , d_enumvalues(NULL)
    {
        /* * */
    }

  private:
    int write_function_interface(Node* n);

    void write_wrapper();
    void write_module();

    // Helper functions
    String* get_attached_typemap(Node* n, const char* tmname, int warning);
    String* get_typemap(Node* n, const char* tmname, SwigType* type, int
                        warning);
    String* get_typemap_out(Node* n, const char* tmname, int warning);
    String* get_typemap(Node* n, const_String_or_char_ptr tmname,
                        SwigType* type, Node* attributes, int warning,
                        const char* suffix);

    bool substitute_classname(SwigType *pt, String *tm);
    void substitute_classname_impl(SwigType *classnametype, String *tm,
                                   const char *classnamespecialvariable);

    void emit_proxy_parm(Node* n, ParmList *l, Wrapper *f);
    void print_carg(String* out, Node* n, String* tm, String* arg);
};

//---------------------------------------------------------------------------//
/*!
 * \brief Main function for code generation.
 */
void FORTRAN::main(int argc, char *argv[])
{
    /* Set language-specific subdirectory in SWIG library */
    SWIG_library_directory("fortran");

    // Set command-line options
    for (int i = 1; i < argc; ++i)
    {
        if ((strcmp(argv[i], "-noproxy") == 0))
        {
            Swig_mark_arg(i);
            d_use_proxy = false;
        }
        else if ((strcmp(argv[i], "-final") == 0))
        {
            Swig_mark_arg(i);
            d_use_final = true;
        }
        else if ((strcmp(argv[i], "-help") == 0))
        {
            Printv(stdout, usage, NULL);
        }
    }

    /* Set language-specific preprocessing symbol */
    Preprocessor_define("SWIGFORTRAN 1", 0);

    /* Set typemap language (historical) */
    SWIG_typemap_lang("fortran");

    /* Set language-specific configuration file */
    SWIG_config_file("fortran.swg");

    /* TODO: fix overloading of types that map to the same value */
    allow_overloading();
    /* TODO: Multiple inheritance? */
    Swig_interface_feature_enable();
}

//---------------------------------------------------------------------------//
/*!
 * \brief Top-level code generation function.
 */
int FORTRAN::top(Node *n)
{
    // Module name (from the SWIG %module command)
    d_module = Getattr(n, "name");
    // Output file name
    d_outpath = Getattr(n, "outfile");

    /* Initialize temporary file-like output strings */

    // run time code (beginning of .cxx file)
    f_begin = NewString("");
    Swig_register_filebyname("begin", f_begin);

    // run time code (beginning of .cxx file)
    f_runtime = NewString("");
    Swig_register_filebyname("runtime", f_runtime);

    // header code (after run time)
    f_header = NewString("");
    Swig_register_filebyname("header", f_header);

    // C++ wrapper code (middle of .cxx file)
    f_wrapper = NewString("");
    Swig_register_filebyname("wrapper", f_wrapper);

    // initialization code (end of .cxx file)
    f_init = NewString("");
    Swig_register_filebyname("init", f_init);

    // Other imported fortran modules
    f_imports = NewString("");
    Swig_register_filebyname("fimports", f_imports);

    // Public interface functions
    f_public = NewString("");
    Swig_register_filebyname("fpublic", f_public);

    // Fortran classes
    f_types = NewString("");
    Swig_register_filebyname("ftypes", f_types);

    // Fortran class constructors
    f_interfaces = NewString("");
    Swig_register_filebyname("finterfaces", f_interfaces);

    // Fortran subroutines (proxy code)
    f_proxy = NewString("");
    Swig_register_filebyname("fproxy", f_proxy);

    // Tweak substitution code
    Swig_name_register("wrapper", "swigc_%f");
    Swig_name_register("set", "set_%n%v");
    Swig_name_register("get", "get_%n%v");

    d_overloads = NewHash();

    /* Emit all other wrapper code */
    Language::top(n);

    /* Write fortran module files */
    write_wrapper();
    write_module();

    // Clean up files and other data
    Delete(d_overloads);
    Delete(f_proxy);
    Delete(f_interfaces);
    Delete(f_types);
    Delete(f_public);
    Delete(f_imports);
    Delete(f_init);
    Delete(f_wrapper);
    Delete(f_header);
    Delete(f_runtime);
    Delete(f_begin);

    return SWIG_OK;
}

//---------------------------------------------------------------------------//
/*!
 * \brief Write C++ wrapper code
 */
void FORTRAN::write_wrapper()
{
    // Open file
    File* out = NewFile(d_outpath, "w", SWIG_output_files());
    if (!out)
    {
        FileErrorDisplay(d_outpath);
        SWIG_exit(EXIT_FAILURE);
    }

    // Write SWIG auto-generation banner
    Swig_banner(out);

    // Write three different levels of output
    Dump(f_begin,    out);
    Dump(f_runtime,  out);
    Dump(f_header,   out);

    // Write wrapper code
    Printf(out, "#ifdef __cplusplus\n");
    Printf(out, "extern \"C\" {\n");
    Printf(out, "#endif\n");
    Dump(f_wrapper, out);
    Printf(out, "#ifdef __cplusplus\n");
    Printf(out, "}\n");
    Printf(out, "#endif\n");

    // Write initialization code
    Wrapper_pretty_print(f_init, out);

    // Close file
    Delete(out);
}

//---------------------------------------------------------------------------//
/*!
 * \brief Write Fortran implementation module
 */
void FORTRAN::write_module()
{
    // Open file
    String* path = NewStringf(
            "%s%s.f90", SWIG_output_directory(), Char(d_module));
    File* out = NewFile(path, "w", SWIG_output_files());
    if (!out)
    {
        FileErrorDisplay(path);
        SWIG_exit(EXIT_FAILURE);
    }
    Delete(path);

    // Write SWIG auto-generation banner
    Swig_banner_target_lang(out, "!");

    // Write module
    Printv(out, "module ", d_module, "\n"
                " use, intrinsic :: ISO_C_BINDING\n",
                f_imports,
                " implicit none\n"
                "\n"
                " ! PUBLIC METHODS AND TYPES\n",
                f_public, NULL);

    // Write overloads
    for (Iterator kv = First(d_overloads); kv.key; kv = Next(kv))
    {
        const char* prepend_comma = "";
        Printv(out, " public :: ", kv.key, "\n"
                    " interface ", kv.key, "\n"
                    "  module procedure :: ", NULL);

        // Write overloaded procedure names
        for (Iterator it = First(kv.item); it.item; it = Next(it))
        {
            Printv(out, prepend_comma, it.item, NULL);
            prepend_comma = ", ";
        }
        Printv(out, "\n"
                    " end interface\n", NULL);
    }

    Printv(out, " ! TYPES\n",
                f_types,
                "\n"
                " ! WRAPPER DECLARATIONS\n"
                " private\n"
                " interface\n",
                f_interfaces,
                " end interface\n"
                "\n"
                "contains\n"
                "  ! FORTRAN PROXY CODE\n",
                f_proxy,
                "end module ", d_module, "\n",
                NULL);

    // Close file
    Delete(out);
}

//---------------------------------------------------------------------------//
/*!
 * \brief Wrap basic functions.
 *
 * This is also passed class methods via memberfunctionHandler.
 */
int FORTRAN::functionWrapper(Node *n)
{
    // Basic attributes
    String* symname    = Getattr(n, "sym:name");
    ParmList* parmlist = Getattr(n, "parms");

    // >>> INITIALIZE

    // Create wrapper name, taking into account overloaded functions
    String* wname = Copy(Swig_name_wrapper(symname));
    const bool is_overloaded = Getattr(n, "sym:overloaded");
    if (is_overloaded)
    {
        Append(wname, Getattr(n, "sym:overname"));
    }
    else
    {
        if (!addSymbol(symname, n))
            return SWIG_ERROR;
    }

    // Create name of Fortran proxy subroutine/function
    String* fname = NULL;
    if (is_wrapping_class())
    {
        fname = NewStringf("swigf_%s", symname);
        if (is_overloaded)
        {
            Append(fname, Getattr(n, "sym:overname"));
        }
    }
    else
    {
        // Use actual symbolic function name
        fname = Copy(symname);
        if (is_overloaded)
        {
            Append(fname, Getattr(n, "sym:overname"));
        }
    }
    Setattr(n, "wrap:name",  wname);
    Setattr(n, "wrap:fname", fname);

    // Update parameter names for static variables
    // Otherwise, argument names will be like "BaseClass::i"
    String* staticName = Getattr(n, "staticmembervariableHandler:sym:name");
    if (staticName && ParmList_len(parmlist))
    {
        assert(ParmList_len(parmlist) == 1);
        Setattr(parmlist, "name", staticName);
    }

    // A new wrapper function object for the C code, the interface code
    // (Fortran declaration of C function interface), and the Fortran code
    Wrapper* cfunc = NewWrapper();
    Wrapper* imfunc = NewFortranWrapper();
    Wrapper* ffunc = NewFortranWrapper();

    // Separate intermediate block for dummy arguments
    String* imargs = NewString("   use, intrinsic :: ISO_C_BINDING\n");
    String* fargs  = Copy(imargs);
    // String for calling the wrapper on the fortran side (the "action")
    String* fcall  = NewString("");

    // >>> RETURN TYPE

    // Constructors (which to SWIG is a function that returns a 'new'
    // variable) gets turned into a subroutine with the dummy 'this'
    // parameter that we bind to the result of the 'new' function
    String* c_return_type  = get_typemap_out(
            n, "ctype", WARN_FORTRAN_TYPEMAP_CTYPE_UNDEF);
    String* im_return_type = get_typemap_out(
            n, "imtype", WARN_FORTRAN_TYPEMAP_IMTYPE_UNDEF);
    String* f_return_type  = get_typemap_out(
            n, "ftype", WARN_FORTRAN_TYPEMAP_FTYPE_UNDEF);
    Setattr(n, "wrap:type",   c_return_type);
    Setattr(n, "wrap:imtype", im_return_type);
    Setattr(n, "wrap:ftype",  f_return_type);

    // Check whether the C routine returns a variable
    const bool is_csubroutine = (Cmp(c_return_type, "void") == 0);
    // Check whether the Fortran routine returns a variable
    const bool is_fsubroutine = (Len(f_return_type) == 0);

    const char* im_func_type = (is_csubroutine ? "subroutine" : "function");
    const char* f_func_type  = (is_fsubroutine ? "subroutine" : "function");

    Printv(cfunc->def, "SWIGEXPORT ", c_return_type, " ", wname, "(", NULL);
    Printv(imfunc->def, im_func_type, " ", fname, "(", NULL);
    Printv(ffunc->def,  f_func_type,  " ", wname, "(", NULL);

    if (!is_csubroutine)
    {
        // Add local variables for result
        Wrapper_add_localv(cfunc, "fresult",
                           c_return_type, "fresult = 0", NULL);
        Wrapper_add_localv(ffunc,  "fresult",
                           im_return_type, " :: fresult", NULL);

        // Add dummy variable for intermediate return value
        Printv(imargs, im_return_type, " :: fresult\n", NULL);

        // Call function and set intermediate result
        Printv(fcall, "fresult = ", wname, "(", NULL);
    }
    else
    {
        Printv(fcall, "call ", wname, "(", NULL);
    }

    if (!is_fsubroutine)
    {
        // Add dummy variable for Fortran proxy return
        Printv(fargs, f_return_type, " :: swigf_result\n", NULL);
    }

    // >>> FUNCTION PARAMETERS/ARGUMENTS

    // Emit all of the local variables for holding arguments.
    emit_parameter_variables(parmlist, cfunc);
    Swig_typemap_attach_parms("ctype",  parmlist, cfunc);
    emit_attach_parmmaps(parmlist, cfunc);
    Setattr(n, "wrap:parms", parmlist);

    // Emit local variables in fortran code
    emit_proxy_parm(n, parmlist, ffunc);
    
    // TODO: change to a typemap??
    if (String* fargs_prepend = Getattr(n, "fortran:argprepend"))
    {
        // Add comma if additional arguments will be added
        Printv(ffunc->def, fargs_prepend,
              (Len(parmlist) > 0 ? ", " : ""), NULL);
    }

    // >>> BUILD WRAPPER FUNCTION AND INTERFACE CODE

    const char* prepend_comma = "";
    Parm* p = parmlist;
    while (p)
    {
        while (p && checkAttribute(p, "tmap:in:numinputs", "0"))
        {
            p = Getattr(p, "tmap:in:next");
        }
        if (!p)
        {
            // It's possible that the last argument is ignored
            break;
        }

        // >>> C ARGUMENTS

        // Name of the argument in the function call (e.g. farg1)
        String* imarg = Getattr(p, "imname");

        // Get the C type
        String* tm = get_attached_typemap(p, "ctype",
                                          WARN_FORTRAN_TYPEMAP_CTYPE_UNDEF);

        Printv(cfunc->def, prepend_comma, NULL);
        this->print_carg(cfunc->def, n, tm, imarg);

        // >>> C ARGUMENT CONVERSION

        String* tm_in = get_attached_typemap(p, "in", WARN_TYPEMAP_IN_UNDEF);
        Replaceall(tm_in, "$input", imarg);
        Setattr(p, "emit:input", imarg);
        Printv(cfunc->code, tm_in, "\n", NULL);

        // >>> F WRAPPER ARGUMENTS

        // Add parameter name to declaration list
        Printv(imfunc->def, prepend_comma, imarg, NULL);

        // XXX simplify get_typemap call -- shouldn't these already be bound?
        // why are we passing the function node as the first argument?

        // Add dummy argument to wrapper body
        String* imtype = get_typemap(p, "imtype", Getattr(n, "type"), p,
                                     WARN_FORTRAN_TYPEMAP_IMTYPE_UNDEF, "in");
        Printv(imargs, "   ", imtype, " :: ", imarg, "\n", NULL);
        Printv(fcall, prepend_comma, imarg, NULL);
        
        // >>> F PROXY ARGUMENTS

        // Add parameter name to declaration list
        String* farg = Getattr(p, "fname");
        Printv(ffunc->def, prepend_comma, farg, NULL);

        // Add dummy argument to wrapper body
        String* ftype = get_typemap(p, "ftype", Getattr(n, "type"), p,
                                    WARN_FORTRAN_TYPEMAP_FTYPE_UNDEF, "in");
        Printv(fargs, "   ", ftype, " :: ", farg, "\n", NULL);

        // >>> F PROXY CONVERSION

        tm_in = get_attached_typemap(p, "fin", WARN_TYPEMAP_IN_UNDEF);
        Replaceall(tm_in, "$input", farg);
        Printv(ffunc->code, tm_in, "\n", NULL);

        // Next iteration
        prepend_comma = ", ";
        p = nextSibling(p);
    }

    // END FUNCTION DEFINITION
    Printv(cfunc->def,  ") {", NULL);
    Printv(imfunc->def, ") &\n"
           "    bind(C, name=\"", wname, "\")", NULL);
    Printv(ffunc->def,  ")", NULL);
    Printv(fcall, ")", NULL);

    // Save fortran function call action
    Setattr(n, "wrap:faction", fcall);
    Delete(fcall);

    if (!is_csubroutine)
    {
        Printv(imfunc->def, " &\n     result(fresult)\n", NULL);
    }
    else
    {
        Printv(imfunc->def, "\n", NULL);
    }
    if (!is_fsubroutine)
    {
        Printv(ffunc->def, " &\n     result(swigf_result)\n", NULL);
    }
    else
    {
        Printv(ffunc->def, "\n", NULL);
    }

    // Append dummy variables to the function "definition" line (before the
    // code and local variable declarations)
    Printv(imfunc->def, imargs, NULL);
    Printv(ffunc->def,  fargs, NULL);

    // >>> ADDITIONAL WRAPPER CODE

    // Insert constraint checking code
    p = parmlist;
    while (p)
    {
        if (String* tm = Getattr(p, "tmap:check"))
        {
            Replaceall(tm, "$input", Getattr(p, "emit:input"));
            Printv(cfunc->code, tm, "\n", NULL);
            p = Getattr(p, "tmap:check:next");
        }
        else
        {
            p = nextSibling(p);
        }
    }

    // Insert cleanup code
    String *cleanup = NewString("");
    p = parmlist;
    while (p)
    {
        if (String* tm = Getattr(p, "tmap:freearg"))
        {
            Replaceall(tm, "$input", Getattr(p, "emit:input"));
            Printv(cleanup, tm, "\n", NULL);
            p = Getattr(p, "tmap:freearg:next");
        }
        else
        {
            p = nextSibling(p);
        }
    }

    // Insert argument output code
    String *outarg = NewString("");
    p = parmlist;
    while (p)
    {
        if (String* tm = Getattr(p, "tmap:argout"))
        {
            Replaceall(tm, "$result", "fresult");
            Replaceall(tm, "$input", Getattr(p, "emit:input"));
            Printv(outarg, tm, "\n", NULL);
            p = Getattr(p, "tmap:argout:next");
        }
        else
        {
            p = nextSibling(p);
        }
    }

    // Generate code to make the C++ function call, convert the  
    Swig_director_emit_dynamic_cast(n, cfunc);
    String* actioncode = emit_action(n);

    SwigType* cpp_returntype = Getattr(n, "type");
    if (String* code = Swig_typemap_lookup_out(
                    "out", n, Swig_cresult_name(), cfunc, actioncode))
    {
        // Output typemap is defined; emit the function call and result
        // conversion code
        Replaceall(code, "$result", "fresult");
        Replaceall(code, "$owner", (GetFlag(n, "feature:new") ? "1" : "0"));
        Printv(cfunc->code, code, "\n", NULL);
    }
    else
    {
        Swig_warning(WARN_TYPEMAP_OUT_UNDEF, input_file, line_number,
                     "Unable to use return type %s in function %s.\n",
                     SwigType_str(cpp_returntype, 0), Getattr(n, "name"));
    }
    emit_return_variable(n, cpp_returntype, cfunc);

    // Emit code to make the Fortran function call in the proxy code
    String* factioncode = Getattr(n, "feature:faction");
    if (!factioncode)
    {
        factioncode = Getattr(n, "wrap:faction");
    }
    Printv(ffunc->code, factioncode, "\n", NULL);

    if (String* code = Swig_typemap_lookup("fout", n, "fresult", ffunc))
    {
        // Output typemap is defined; emit the function call and result
        // conversion code
        Replaceall(code, "$result", (is_fsubroutine ? "" : "swigf_result"));
        Replaceall(code, "$owner", (GetFlag(n, "feature:new") ? "1" : "0"));
        Printv(ffunc->code, code, "\n", NULL);
    }
    else
    {
        Swig_warning(WARN_FORTRAN_TYPEMAP_FOUT_UNDEF, input_file, line_number,
                     "Unable to use return type %s in function %s.\n",
                     SwigType_str(cpp_returntype, 0), Getattr(n, "name"));
    }
    
    // Output argument output and cleanup code
    Printv(cfunc->code, outarg, NULL);
    Printv(cfunc->code, cleanup, NULL);

    if (!is_csubroutine)
    {
        String* qualified_return = SwigType_rcaststr(c_return_type, "fresult");
        Printf(cfunc->code, "    return %s;\n", qualified_return);
        Delete(qualified_return);
    }

    Printf(cfunc->code, "}\n");
    Printv(imfunc->code, "  end ", im_func_type, NULL);
    Printv(ffunc->code,  "  end ", f_func_type, NULL);

    // Apply Standard SWIG substitutions
    Replaceall(cfunc->code, "$cleanup", cleanup);
    Replaceall(cfunc->code, "$symname", symname);
    Replaceall(cfunc->code, "SWIG_contract_assert(",
               "SWIG_contract_assert($null, ");
    Replaceall(cfunc->code, "$null", (is_csubroutine ? "" : "0"));

    // Apply Standard SWIG substitutions
    Replaceall(ffunc->code, "$symname", symname);

    // Write the C++ function into the wrapper code file
    Wrapper_print(cfunc,  f_wrapper);
    Wrapper_print(imfunc, f_interfaces);
    Wrapper_print(ffunc,  f_proxy);
    DelWrapper(cfunc);
    DelWrapper(imfunc);
    DelWrapper(ffunc);

    Delete(outarg);
    Delete(cleanup);
    Delete(fcall);
    Delete(fargs);
    Delete(imargs);
    Delete(fname);
    Delete(wname);

    return write_function_interface(n);
}

//---------------------------------------------------------------------------//
/*!
 * \brief Write the interface/alias code for a wrapped function.
 */
int FORTRAN::write_function_interface(Node* n)
{
    String* fname = Getattr(n, "wrap:fname");
    assert(fname);

    // >>> DETERMINED WRAPPER NAME

    bool is_static = false;

    // Get modified Fortran member name, defaulting to sym:name
    String* alias = NULL;
    // Temporary for any newly allocated string
    String* new_alias = NULL;

    if ((alias = Getattr(n, "fortran:membername")))
    {
        // We've already overridden the member name
    }
    else if ((alias = Getattr(n, "staticmembervariableHandler:sym:name")))
    {
        // Member variable, rename the methods to set_X or get_X
        // instead of set_Class_X or get_Class_X
        is_static = true;

        if (Getattr(n, "varset"))
        {
            alias = new_alias = Swig_name_set(getNSpace(), alias);
        }
        else if (Getattr(n, "varget"))
        {
            alias = new_alias = Swig_name_get(getNSpace(), alias);
        }
        else
        {
            Printv(stderr, "Static member isn't setter or getter:\n", NULL);
            Swig_print_node(n);
        }
    }
    else if ((alias = Getattr(n, "staticmemberfunctionHandler:sym:name")))
    {
        is_static = true;
    }
    else if ((alias = Getattr(n, "membervariableHandler:sym:name")))
    {
        if (Getattr(n, "memberset"))
        {
            alias = new_alias = Swig_name_set(getNSpace(), alias);
        }
        else if (Getattr(n, "memberget"))
        {
            alias = new_alias = Swig_name_get(getNSpace(), alias);
        }
        else
        {
            // Standard class method
            alias = Getattr(n, "sym:name");
        }
    }
    else
    {
        alias = Getattr(n, "sym:name");
    }

    // >>> WRITE FUNCTION WRAPPER

    const bool is_overloaded = Getattr(n, "sym:overloaded");
    if (is_wrapping_class())
    {
        if (is_overloaded)
        {
            // Create overloaded aliased name
            String* overalias = Copy(alias);
            Append(overalias, Getattr(n, "sym:overname"));

            // Add name to method overload list
            List* overloads = Getattr(d_method_overloads, alias);
            if (!overloads)
            {
                overloads = NewList();
                Setattr(d_method_overloads, alias, overloads);
            }
            Append(overloads, overalias);

            alias = overalias;
        }

        Printv(f_types,
               "  procedure", (  is_static     ? ", nopass"
                               : is_overloaded ? ", private"
                               : ""),
               " :: ", alias, " => ", fname, "\n",
               NULL);
    }
    else 
    {
        // Not a class: make the function public (and alias the name)
        if (is_overloaded)
        {
            // Append this function name to the list of overloaded names for the
            // symbol. 'public' access specification gets added later.
            List* overloads = Getattr(d_overloads, alias);
            if (!overloads)
            {
                overloads = NewList();
                Setattr(d_overloads, alias, overloads);
            }
            Append(overloads, Copy(fname));
        }
        else
        {
            Printv(f_public,
                   " public :: ", alias, "\n",
                   NULL);
        }
    }
    Delete(new_alias);
    return SWIG_OK;
}

//---------------------------------------------------------------------------//
/*!
 * \brief Create a friendly parameter name
 */
String* FORTRAN::makeParameterName(Node *n, Parm *p,
                                   int arg_num, bool setter) const
{
    String *name = Getattr(p, "name");
    if (name)
        return Swig_name_make(p, 0, name, 0, 0);

    // The general function which replaces arguments whose
    // names clash with keywords with (less useful) "argN".
    return Language::makeParameterName(n, p, arg_num, setter);
}

//---------------------------------------------------------------------------//
/*!
 * \brief Process classes.
 */
int FORTRAN::classHandler(Node *n)
{
    // Basic attributes
    String* symname = Getattr(n, "sym:name");
    String* basename = NULL;

    if (!addSymbol(symname, n))
        return SWIG_ERROR;

    // Process base classes
    List *baselist = Getattr(n, "bases");
    if (baselist && Len(baselist) > 0)
    {
        Swig_warning(WARN_LANG_NATIVE_UNIMPL, Getfile(n), Getline(n),
                "Inheritance (class '%s') support is under development and "
                "limited.\n",
                SwigType_namestr(symname));
        Node* base = Getitem(baselist, 0);
        basename = Getattr(base, "sym:name");
    }
    if (baselist && Len(baselist) > 1)
    {
        Swig_warning(WARN_LANG_NATIVE_UNIMPL, Getfile(n), Getline(n),
                "Multiple inheritance (class '%s') is not supported in Fortran",
                "\n",
                SwigType_namestr(symname));
    }

    // Initialize output strings that will be added by 'functionHandler'
    d_method_overloads = NewHash();

    // Write Fortran class header
    d_classtype = Getattr(n, "classtypeobj");

    // Make the class publicly accessible
    Printv(f_public, " public :: ", symname, "\n",
                    NULL);

    Printv(f_types, " type", NULL);
    if (basename)
    {
        Printv(f_types, ", extends(", basename, ")", NULL);
    }

    if (Abstract)
    {
        // The 'Abstract' global variable is set to 1 if this class is abstract
        Printv(f_types, ", abstract", NULL);
    }

    Printv(f_types, " :: ", symname, "\n", NULL);

    // Insert the class data. Only do this if the class has no base classes
    if (!basename)
    {
        Printv(f_types, "  ", lstrip(get_typemap(n, "fdata", d_classtype,
                                 WARN_FORTRAN_TYPEMAP_FDATA_UNDEF)),
        NULL);
    }
    Printv(f_types, " contains\n", NULL);

    // Emit class members
    Language::classHandler(n);

    // Add assignment operator for smart pointers
    String* spclass = Getattr(n, "feature:smartptr");
    if (spclass)
    {
        // Create overloaded aliased name
        String* alias      = NewString("assignment(=)");
        String* fname = NewStringf("swigf_assign_%s",
                                        Getattr(n, "sym:name"));
        String* wrapname = NewStringf("swigc_spcopy_%s",
                                      Getattr(n, "sym:name"));

        // Add self-assignment to method overload list
        assert(!Getattr(d_method_overloads, alias));
        List* overloads = NewList();
        Setattr(d_method_overloads, alias, overloads);
        Append(overloads, fname);

        // Define the method
        Printv(f_types,
               "  procedure, private :: ", fname, "\n",
               NULL);
        
        // Add the proxy code implementation of assignment
        Printv(f_proxy,
           "  subroutine ", fname, "(self, other)\n"
           "   use, intrinsic :: ISO_C_BINDING\n"
           "   class(", symname, "), intent(inout) :: self\n"
           "   type(", symname, "), intent(in) :: other\n"
           "   call self%release()\n"
           "   self%swigptr = ", wrapname, "(other%swigptr)\n"
           "  end subroutine\n",
           NULL);

        // Add interface code
        Printv(f_interfaces,
               "  function ", wrapname, "(farg1) &\n"
               "     bind(C, name=\"", wrapname, "\") &\n"
               "     result(fresult)\n"
               "   use, intrinsic :: ISO_C_BINDING\n"
               "   type(C_PTR) :: fresult\n"
               "   type(C_PTR), value :: farg1\n"
               "  end function\n",
               NULL);

        // Add C code
        Wrapper* cfunc = NewWrapper();
        Printv(cfunc->def, "SWIGEXPORT void* ", wrapname, "(void* farg1) {\n",
               NULL);
        Printv(cfunc->code, spclass, "* arg1 = (", spclass, " *)farg1;\n"
                       ""
                       "    return new ", spclass, "(*arg1);\n"
                       "}\n",
                       NULL);
        Wrapper_print(cfunc, f_wrapper);

        Delete(alias);
        Delete(fname);
        Delete(wrapname);
        DelWrapper(cfunc);
    }

    // Write overloads
    for (Iterator kv = First(d_method_overloads); kv.key; kv = Next(kv))
    {
        Printv(f_types, "  generic :: ", kv.key, " => ", NULL);
        // Note: subtract 2 becaues this first line is an exception to
        // prepend_comma, added inside the iterator
        int line_length = 13 + Len(kv.key) + 4 - 2;

        // Write overloaded procedure names
        print_wrapped_line(f_types, First(kv.item), line_length);
        Printv(f_types, "\n", NULL);
    }

    // Close out the type
    Printv(f_types, " end type\n",
                    NULL);

    Delete(d_method_overloads);
    d_classtype = NULL;

    return SWIG_OK;
}

//---------------------------------------------------------------------------//
/*!
 * \brief Extra stuff for constructors.
 */
int FORTRAN::constructorHandler(Node* n)
{
    Node *classn = getCurrentClass();
    assert(classn);

    // Possibly renamed constructor (default: name of the class)
    String* symname = Getattr(n, "sym:name");
    String* classname = Getattr(classn, "sym:name");

    const_String_or_char_ptr alias = "create";
    if (Cmp(symname, classname))
    {
        // User provided a custom name (it differs from the class name)
        // Printf(stderr, "User aliased constructor name %s => %s\n",
        //        Getattr(classn, "sym:name"), symname);
        alias = symname;

        // To avoid conflicts with templated functions, modify the
        // constructor's symname
        String* mrename = NewStringf("%s_%s", classname, symname);
        Setattr(n, "sym:name", mrename);
        Delete(mrename);
    }
    Setattr(n, "fortran:membername", alias);

    // Replace Fortran return type with void
    Setattr(n, "ftype:out", "");

    // Replace standard "out" typemap with custom 
    String* fcreate = get_typemap(n, "fcreate",
                           d_classtype,
                           WARN_FORTRAN_TYPEMAP_FCREATE_UNDEF);
    if (fcreate)
    {
        Setattr(n, "fout", fcreate);
    }

    // Add an extra "self" argument to the wrapper code
    Setattr(n, "fortran:argprepend", "self");
    
    Language::constructorHandler(n);

    return SWIG_OK;
}

//---------------------------------------------------------------------------//
/*!
 * \brief Handle extra destructor stuff.
 */
int FORTRAN::destructorHandler(Node* n)
{
    Setattr(n, "fortran:membername", "release");

    // Replace standard "out" typemap with custom 
    String* frelease = get_typemap(n, "frelease",
                           d_classtype,
                           WARN_FORTRAN_TYPEMAP_FRELEASE_UNDEF);
    if (frelease)
    {
        Setattr(n, "fout", frelease);
    }
    
    Language::destructorHandler(n);

    // XXX turn final into a feature and change to typemaps
    if (d_use_final)
    {
        // Create 'final' name wrapper
        String* fname = NewStringf("swigf_final_%s",
                                        Getattr(n, "sym:name"));
        String* classname = Getattr(getCurrentClass(), "sym:name");

        // Add the 'final' subroutine to the methods
        Printv(f_types, "  final     :: ", fname, "\n",
               NULL);

        // Add the 'final' implementation
        Printv(f_proxy,
           "  subroutine ", fname, "(self)\n"
           "   use, intrinsic :: ISO_C_BINDING\n"
           "   class(", classname, ") :: self\n"
           "   call ", Getattr(n, "wrap:name"), "(self%swigptr)\n"
           "   self%swigptr = C_NULL_PTR\n"
           "  end subroutine\n",
           NULL);

        // Add implementation
        Delete(fname);
    }

    return SWIG_OK;
}

//---------------------------------------------------------------------------//
/*!
 * \brief Process member functions.
 */
int FORTRAN::memberfunctionHandler(Node *n)
{
    Setattr(n, "fortran:membername", Getattr(n, "sym:name"));
    Language::memberfunctionHandler(n);
    return SWIG_OK;
}

//---------------------------------------------------------------------------//
/*!
 * \brief Process an '%import' directive.
 *
 * Besides importing typedefs, this should add a "use MODULENAME" line inside
 * the "module" block of the proxy code (before the "contains" line).
 */
int FORTRAN::importDirective(Node *n)
{
    String* modname = Getattr(n, "module");
    if (modname)
    {
        // The actual module contents should be the first child
        // of the provided %import node 'n'.
        Node* mod = firstChild(n);
        // Swig_print_node(mod);
        assert(Strcmp(nodeType(mod), "module") == 0);

        // I don't know if the module name could ever be different from the
        // 'module' attribute of the import node, but just in case... ?
        modname = Getattr(mod, "name");
        Printv(f_imports, " use ", modname, "\n", NULL);
    }

    return Language::importDirective(n);
}

//---------------------------------------------------------------------------//
/*!
 * \brief Process an '%insert' directive.
 *
 * This allows us to do custom insertions into parts of the fortran module.
 */
int FORTRAN::insertDirective(Node *n)
{
    if (ImportMode)
        return Language::insertDirective(n);

    String *code = Getattr(n, "code");
    String *section = Getattr(n, "section");

    // Make sure the code ends its line
    Append(code, "\n");
      
    if (Cmp(section, "fortran") == 0)
    {
        if (d_use_proxy)
        {
            if (is_wrapping_class())
            {
                substitute_classname(d_classtype, code);
            }
            
            // Insert code into the body of the module (after "contains")
            Printv(f_proxy, code, NULL);
        }
    }
    else if (Cmp(section, "fortranspec") == 0)
    {
        if (is_wrapping_class())
        {
            // Insert code into the class definition
            substitute_classname(d_classtype, code);
            Printv(f_types, code, NULL);
        }
        else
        {
            // Insert code into the header of the module (alongside "public"
            // methods), used for adding 'ierr' to the module contents
            Printv(f_public, code, NULL);
        }
    }
    else
    {
        return Language::insertDirective(n);
    }

    return SWIG_OK;
}

//---------------------------------------------------------------------------//
/*!
 * \brief Wrap an enum declaration
 *
 */
int FORTRAN::enumDeclaration(Node *n)
{
    if (ImportMode)
      return SWIG_OK;

    // Symname is not present if the enum is not being wrapped
    // (protected/private)
    // XXX: do we also need to check for 'ignore'?
    String* symname = Getattr(n, "sym:name");

    if (symname)
    {
        // Scope the enum if it's in a class
        String* enum_name = NULL;
        if (is_wrapping_class())
        {
            enum_name = NewStringf("%s_%s", getClassName(), symname);
        }
        else
        {
            enum_name = Copy(symname);
        }

        // Print the enumerator with a placeholder so we can use 'kind(ENUM)'
        Printv(f_types, " enum, bind(c)\n",
                        "  enumerator :: ", enum_name, " = -1\n", NULL);

        d_enumvalues = NewList();
        Append(d_enumvalues, enum_name);
        Delete(enum_name);
    }

    // Emit enum items
    Language::enumDeclaration(n);

    if (symname)
    {
        // End enumeration
        Printv(f_types, " end enum\n", NULL);

        // Make the enum class *and* its values public
        Printv(f_public, " public :: ", NULL);
        print_wrapped_line(f_public, First(d_enumvalues), 11);
        Printv(f_public, "\n", NULL);
        Delete(d_enumvalues);
        d_enumvalues = NULL;
    }

    return SWIG_OK;
}

//---------------------------------------------------------------------------//
/*!
 * \brief Wrap a value in an enum
 *
 * This is called inside enumDeclaration
 */
int FORTRAN::enumvalueDeclaration(Node *n)
{
    Language::enumvalueDeclaration(n);
    String* name = Getattr(n, "sym:name");
    String* value = Getattr(n, "enumvalue");

    if (!value)
    {
        // Implicit enum value (no value specified: PREVIOUS + 1)
        value = Getattr(n, "enumvalueex");
    }

    if (name && value)
    {
        if (d_enumvalues)
        {
            Append(d_enumvalues, name);
            Printv(f_types, "  enumerator :: ", name, " = ", value, "\n", NULL);
        }
        else
        {
            // Anonymous enum (TODO: change to parameter??)
            Swig_warning(WARN_LANG_NATIVE_UNIMPL, Getfile(n), Getline(n),
                    "Anonymous enums ('%s') are currently unsupported "
                    "and will not be wrapped\n",
                    SwigType_namestr(name));
        }
    }
    else
    {
        Printv(stderr, "Enum is missing a name or value:", NULL);
        Swig_print_node(n);
    }

    return SWIG_OK;
}

//---------------------------------------------------------------------------//
// HELPER FUNCTIONS
//---------------------------------------------------------------------------//
/*!
 * Get a typemap that should already be attached.
 *
 * This can be called if 'get_typemap' was applied to the given node already, or
 * e.g. if Swig_typemap_attach_parms was called.
 */
String* FORTRAN::get_attached_typemap(
        Node*        n,
        const char*  tmname,
        int          warning)
{
    return get_typemap(n, tmname, Getattr(n, "type"), NULL, warning, NULL);
}

//---------------------------------------------------------------------------//
/*!
 * Get a typemap from the current node.
 */
String* FORTRAN::get_typemap(
        Node*        n,
        const char*  tmname,
        SwigType*    type,
        int          warning)
{
    assert(n);
    assert(type);
    Hash* attributes = NewHash();
    String* result = get_typemap(n, tmname, type, attributes, warning, NULL);
    Delete(attributes);
    return result;
}

//---------------------------------------------------------------------------//
/*!
 * Get a typemap from a given type, overriding with 'out'.
 */
String* FORTRAN::get_typemap_out(
        Node*        n,
        const char*  tmname,
        int          warning)
{
    assert(n);
    return get_typemap(n, tmname, Getattr(n, "type"), n, warning, "out");
}

//---------------------------------------------------------------------------//
/*!
 * \brief Returns a new string for a typemap that accepts no arguments
 *
 * If 'attributes' is null, we assume the typemap has already been bound.
 * Otherwise we call Swig_typemap_lookup to bind to the given attributes.
 *
 * If 'warning' is WARN_NONE, then if the typemap is not found, the return
 * value will be NULL. Otherwise a mangled typename will be created and saved
 * to attributes (or if attributes is null, then the given node).
 *
 * If 'suffix' is non-null, then after binding, a search will be made for the
 * typemap with the given suffix. If that's present, it's used instead of the
 * default typemap. (This allows overriding of e.g. 'tmap:ctype' with
 * 'tmap:ctype:out'.)
 */
String* FORTRAN::get_typemap(
        Node*                     n,
        const_String_or_char_ptr  tmname,
        SwigType*                 type,
        Node*                     attributes,
        int                       warning,
        const char*               suffix)
{
    String* tm = NULL;
    String* key = NULL;
    if (attributes)
    {
        // Bind the typemap to this node
        Setattr(attributes, "type", type);
        Setfile(attributes, Getfile(n));
        Setline(attributes, Getline(n));
        const char lname[] = "";
        tm = Swig_typemap_lookup(tmname, attributes, lname, NULL);
    }
    else
    {
        // Look up an already-attached typemap
        key = NewStringf("tmap:%s", tmname);
        tm = Getattr(n, key);
    }

    if (tm)
    {
        if (suffix)
        {
            // Check for optional override (i.e. tmap:ctype:out)
            if (!key)
            {
                key = NewStringf("tmap:%s:%s", tmname, suffix);
            }
            else
            {
                Printv(key, ":", suffix, NULL);
            }
            String* suffixed_tm = Getattr(n, key);
            if (suffixed_tm)
                tm = suffixed_tm;
        }
    }
    else if (warning != WARN_NONE)
    {
        SwigType *type = Getattr(n, "type");
        tm = NewStringf("SWIGTYPE%s", SwigType_manglestr(type));
        Swig_warning(warning,
                     Getfile(n), Getline(n),
                     "No '%s' typemap defined for %s\n", tmname,
                     SwigType_str(type, 0));
        // Save the mangled typemap
        if (!key)
        {
            key = NewStringf("tmap:%s", tmname);
        }
        Setattr((attributes ? attributes : n), key, tm);
    }
    return tm;
}

//---------------------------------------------------------------------------//
// Substitute the '$fclassname' variables with the Fortran proxy class
// wrapper names. Shamelessly stolen from the Java wrapper code.

bool FORTRAN::substitute_classname(SwigType *pt, String *tm)
{
    bool substitution_performed = false;
#if 0
    Printf(stdout, "calling substitute_classname(%s, %s)\n", pt, tm);
#endif

    SwigType *type = Copy(SwigType_typedef_resolve_all(pt));
    SwigType *strippedtype = SwigType_strip_qualifiers(type);

    if (Strstr(tm, "$fclassname"))
    {
        substitute_classname_impl(strippedtype, tm, "$fclassname");
        substitution_performed = true;
    }

#if 0
    Printf(stdout, "substitute_classname (%c): %s => %s\n",
           substitution_performed ? 'X' : ' ',
           pt, strippedtype);
#endif

    Delete(strippedtype);
    Delete(type);

    return substitution_performed;
}

//---------------------------------------------------------------------------//

void FORTRAN::substitute_classname_impl(SwigType *classnametype, String *tm,
                                        const char *classnamespecialvariable)
{
    String *replacementname = NULL;

    if (SwigType_isenum(classnametype))
    {
        Node *lookup = enumLookup(classnametype);
        if (lookup)
        {
            replacementname = Getattr(lookup, "sym:name");
        }
    }
    else
    {
        Node* lookup = classLookup(classnametype);
        if (lookup)
        {
            replacementname = Getattr(lookup, "sym:name");
        }
    }

    if (replacementname)
    {
        Replaceall(tm, classnamespecialvariable, replacementname);
    }
    else
    {
        // use $descriptor if SWIG does not know anything about this type.
        // Note that any typedefs are resolved.
        Swig_warning(WARN_FORTRAN_TYPEMAP_FTYPE_UNDEF,
                     input_file, line_number,
                     "No '$fclassname' replacement (wrapped type) "
                     "found for %s\n",
                     SwigType_str(classnametype, 0));

        replacementname = NewStringf("SWIGTYPE%s",
                                     SwigType_manglestr(classnametype));

        Replaceall(tm, classnamespecialvariable, replacementname);
        Delete(replacementname);
    }
}

//---------------------------------------------------------------------------//

void FORTRAN::emit_proxy_parm(Node* n, ParmList *parmlist, Wrapper *f)
{
    // Bind wrapper typemaps to parameter arguments
    Swig_typemap_attach_parms("imtype", parmlist, f);
    Swig_typemap_attach_parms("ftype",  parmlist, f);
    Swig_typemap_attach_parms("fin",    parmlist, f);

    // Emit parameters
    Parm *p = parmlist;
    int i = 0;
    while (p)
    {
        while (p && checkAttribute(p, "tmap:in:numinputs", "0"))
        {
            p = Getattr(p, "tmap:in:next");
            ++i;
        }
        if (!p)
        {
            // It's possible that the last argument is ignored
            break;
        }

        // Set fortran intermediate name
        String* imarg = NewStringf("f%s", Getattr(p, "lname"));
        Setattr(p, "imname", imarg);

        // Local parameter 
        String* imtype = get_attached_typemap(
                p, "imtype", WARN_FORTRAN_TYPEMAP_IMTYPE_UNDEF);

        Wrapper_add_localv(f, imarg,
                           "   ", imtype, " :: ", imarg, NULL);

        String* farg = this->makeParameterName(n, p, i);
        Setattr(p, "fname", farg);

        Delete(farg);
        Delete(imtype);
        Delete(imarg);

        // Next iteration
        p = nextSibling(p);
        ++i;
    }
}

//---------------------------------------------------------------------------//
//! Add a named C argument to a function declaration.
void FORTRAN::print_carg(String* out, Node* n, String* tm, String* arg)
{
    if (!SwigType_isfunctionpointer(Getattr(n, "type")))
    {
        Printv(out, tm, " ", arg, NULL);
    }
    else
    {
        // Function pointer syntax requires special handling:
        // Replace (PRVAL) (*)(PARGS) arg with (PRVAL)(*arg)(PARGS)
        String* tm_arg = Copy(tm);
        String* subst = NewStringf("(*%s)(", arg);

        Replace(tm_arg, " (*)(", subst, DOH_REPLACE_FIRST);
        Printv(out, tm_arg, NULL);

        Delete(subst);
        Delete(tm_arg);
    }
}

//---------------------------------------------------------------------------//

void FORTRAN::replaceSpecialVariables(String* method, String* tm, Parm* parm)
{
    (void)method;
    SwigType *type = Getattr(parm, "type");
    this->substitute_classname(type, tm);
}

//---------------------------------------------------------------------------//
// Expose the code to the SWIG main function.
//---------------------------------------------------------------------------//
extern "C" Language *
swig_fortran(void)
{
    return new FORTRAN();
}
