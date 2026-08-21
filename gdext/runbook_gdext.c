/* runbook_gdext.c — the Godot binding.
 *
 * Plain C against gdextension_interface.h, which the engine dumped itself
 * (--dump-gdextension-interface). Zero fetched dependencies, and it cannot
 * drift from the engine version. Lifted from ~/NOMINAL/gdext, which had
 * already fought the versioned-entry-point problem and won.
 *
 * IT IS THREE METHODS, AND THAT IS THE WHOLE POINT.
 *
 * Handoff decision 7: the UI is a client of the API, from commit one. So this
 * file exposes the API and nothing else -- the same proto_exec() the socket
 * speaks, the same one the reference agent plays through. There is no
 * privileged call for the desktop, no struct handed to GDScript, no second
 * way to ask the world anything.
 *
 * That is what makes "find the API" a discovery of something that was always
 * there rather than a feature bolted on for Act II. It also halves the
 * implementation, and it means the client physically cannot do something a
 * player's script could not.
 *
 * If you are about to add a fourth method, ask first whether the answer could
 * be a verb in proto.c instead. It almost always can, and then telnet, the
 * reference agent, --mancheck and the desktop all get it at once.
 */
#include "gdextension_interface.h"

#ifndef GDE_EXPORT
#  if defined(_WIN32)
#    define GDE_EXPORT __declspec(dllexport)
#  else
#    define GDE_EXPORT __attribute__((visibility("default")))
#  endif
#endif

#include "proto.h"
#include "ticket.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static GDExtensionInterfaceGetProcAddress api_get;
static GDExtensionClassLibraryPtr         api_lib;

static GDExtensionInterfaceClassdbRegisterExtensionClass6      classdb_register;
static GDExtensionInterfaceClassdbRegisterExtensionClassMethod classdb_add_method;
static GDExtensionInterfaceStringNewWithUtf8Chars              string_new_utf8;
static GDExtensionInterfaceStringToUtf8Chars                   string_to_utf8;
static GDExtensionInterfaceVariantGetPtrDestructor             variant_get_dtor;
static GDExtensionInterfaceObjectSetInstance                   object_set_instance;
static GDExtensionInterfaceClassdbConstructObject3             classdb_construct;
static GDExtensionInterfaceStringNameNewWithUtf8Chars          stringname_new;
static GDExtensionPtrDestructor  string_destroy;

#define GETPROC(var, name) var = (void *)api_get(name)

/* ------------------------------------------------------------ string glue */
typedef struct { uint8_t opaque[8]; } SN;

static void sn_make(SN *sn, const char *s) { memset(sn, 0, sizeof *sn); stringname_new(sn, s); }

static void gdstring_to_c(const void *gdstr, char *out, size_t outsz)
{
    GDExtensionInt need = string_to_utf8((GDExtensionConstStringPtr)gdstr, NULL, 0);
    if (need < 0) need = 0;
    if ((size_t)need >= outsz) need = (GDExtensionInt)(outsz - 1);
    string_to_utf8((GDExtensionConstStringPtr)gdstr, out, need);
    out[need] = 0;
}

static void c_to_gdstring(void *dest, const char *s)
{
    string_new_utf8((GDExtensionUninitializedStringPtr)dest, s ? s : "");
}

/* ------------------------------------------------------------- the object */
/* One Godot object == one world, with one session onto it. The session
 * carries provenance, and the desktop sets it to `hand`: everything a player
 * does through a form is work done by a person, and the debt mechanic (§11)
 * is built on nothing but that distinction. */
typedef struct {
    Specs  *specs;
    World  *w;
    Session s;
    char    err[RB_ERR_MAX];
} Client;

static SN sn_class, sn_parent;

static void client_boot(Client *c, uint64_t seed)
{
    if (c->w) world_free(c->w);
    c->w = NULL;
    if (!c->specs) {
        c->specs = specs_load(NULL, c->err, sizeof c->err);
        if (!c->specs) return;
    }
    c->w = world_new(seed, c->specs);
    proto_open(&c->s, c->w);
    /* A person at a keyboard. See the note on the struct. */
    c->s.prov = PROV_HAND;
}

static GDExtensionObjectPtr client_create(void *userdata, GDExtensionBool notify_postinitialize)
{
    (void)userdata; (void)notify_postinitialize;
    GDExtensionObjectPtr obj = classdb_construct(&sn_parent);
    Client *c = rb_alloc(sizeof(Client));
    memset(c, 0, sizeof *c);
    client_boot(c, 424242);
    object_set_instance(obj, &sn_class, c);
    return obj;
}

static void client_free(void *userdata, GDExtensionClassInstancePtr instance)
{
    (void)userdata;
    Client *c = (Client *)instance;
    if (!c) return;
    if (c->w) world_free(c->w);
    if (c->specs) specs_free(c->specs);
    rb_free(c);
}

/* ------------------------------------------------------------- the methods */

/* exec(String line) -> String
 *
 * THE ONLY DOOR. Everything the desktop does -- listing tickets, submitting a
 * form, reading an appliance's manual, advancing the day -- is a line of the
 * same protocol a telnet session speaks. */
static void m_exec(Client *c, const GDExtensionConstTypePtr *args, void *ret)
{
    char line[RB_LINE_MAX];
    gdstring_to_c(args[0], line, sizeof line);
    Buf out;
    buf_init(&out);
    if (c->w) proto_exec(&c->s, line, &out);
    else      buf_printf(&out, "-ERR %s\n.\n", c->err[0] ? c->err : "no world");
    c_to_gdstring(ret, out.p ? out.p : "");
    buf_free(&out);
}

/* boot(int seed) -> String — start a new run. */
static void m_boot(Client *c, const GDExtensionConstTypePtr *args, void *ret)
{
    int64_t seed = *(const int64_t *)args[0];
    client_boot(c, (uint64_t)seed);
    char msg[RB_ERR_MAX + 64];
    if (c->w) snprintf(msg, sizeof msg, "%s, day %d, %d users", c->w->org, c->w->day, c->w->active);
    else      snprintf(msg, sizeof msg, "failed: %s", c->err);
    c_to_gdstring(ret, msg);
}

/* ready() -> bool — did the specs load and a world come up? A desktop that
 * cannot say this paints a blank screen and blames the player. */
static void m_ready(Client *c, const GDExtensionConstTypePtr *args, void *ret)
{
    (void)args;
    *(GDExtensionBool *)ret = c->w != NULL;
}

#define MAX_ARGS 2
typedef struct {
    const char *name;
    void (*fn)(Client *, const GDExtensionConstTypePtr *, void *);
    int nargs;
    GDExtensionVariantType argtype[MAX_ARGS];
    GDExtensionVariantType rettype;
} MethodDef;

static const MethodDef METHODS[] = {
    { "exec",  m_exec,  1, { GDEXTENSION_VARIANT_TYPE_STRING }, GDEXTENSION_VARIANT_TYPE_STRING },
    { "boot",  m_boot,  1, { GDEXTENSION_VARIANT_TYPE_INT },    GDEXTENSION_VARIANT_TYPE_STRING },
    { "ready", m_ready, 0, { 0 },                               GDEXTENSION_VARIANT_TYPE_BOOL },
};
#define NMETHODS ((int)(sizeof METHODS / sizeof METHODS[0]))

static void method_ptrcall(void *method_userdata, GDExtensionClassInstancePtr instance,
                           const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret)
{
    const MethodDef *m = (const MethodDef *)method_userdata;
    m->fn((Client *)instance, args, ret);
}

/* The Variant path. Godot calls this when the method is invoked dynamically,
 * which is what GDScript does unless the call is statically typed -- so it has
 * to work, not just the ptrcall fast path. */
static void method_call(void *method_userdata, GDExtensionClassInstancePtr instance,
                        const GDExtensionConstVariantPtr *args, GDExtensionInt argc,
                        GDExtensionVariantPtr ret, GDExtensionCallError *error)
{
    const MethodDef *m = (const MethodDef *)method_userdata;
    if (argc < m->nargs) {
        error->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
        error->argument = (int32_t)m->nargs;
        return;
    }
    error->error = GDEXTENSION_CALL_OK;

    uint8_t raw[MAX_ARGS][64];
    const void *argp[MAX_ARGS];
    for (int i = 0; i < MAX_ARGS; i++) argp[i] = raw[i];
    static GDExtensionInterfaceGetVariantToTypeConstructor to_type;
    if (!to_type) GETPROC(to_type, "get_variant_to_type_constructor");

    for (int i = 0; i < m->nargs; i++) {
        memset(raw[i], 0, sizeof raw[i]);
        GDExtensionTypeFromVariantConstructorFunc conv = to_type(m->argtype[i]);
        conv(raw[i], (GDExtensionVariantPtr)args[i]);
    }

    uint8_t rawret[64];
    memset(rawret, 0, sizeof rawret);
    m->fn((Client *)instance, (const GDExtensionConstTypePtr *)argp, rawret);

    static GDExtensionInterfaceGetVariantFromTypeConstructor from_type;
    if (!from_type) GETPROC(from_type, "get_variant_from_type_constructor");
    GDExtensionVariantFromTypeConstructorFunc back = from_type(m->rettype);
    back(ret, rawret);

    if (m->rettype == GDEXTENSION_VARIANT_TYPE_STRING && string_destroy) string_destroy(rawret);
    for (int i = 0; i < m->nargs; i++)
        if (m->argtype[i] == GDEXTENSION_VARIANT_TYPE_STRING && string_destroy) string_destroy(raw[i]);
}

/* One empty StringName and one empty String, owned for the library's
 * lifetime. Godot reads these while registering and never takes ownership. */
static SN empty_sn;
static struct { uint8_t opaque[8]; } empty_string;

static void register_methods(void)
{
    sn_make(&empty_sn, "");
    c_to_gdstring(&empty_string, "");

    for (int i = 0; i < NMETHODS; i++) {
        const MethodDef *m = &METHODS[i];
        SN mname;
        sn_make(&mname, m->name);

        GDExtensionPropertyInfo ret_info;
        memset(&ret_info, 0, sizeof ret_info);
        ret_info.type = m->rettype;
        ret_info.name = &empty_sn;
        ret_info.class_name = &empty_sn;
        ret_info.hint_string = &empty_string;
        ret_info.usage = 6;

        GDExtensionPropertyInfo args_info[MAX_ARGS];
        GDExtensionClassMethodArgumentMetadata args_meta[MAX_ARGS];
        for (int a = 0; a < m->nargs; a++) {
            memset(&args_info[a], 0, sizeof args_info[a]);
            args_info[a].type = m->argtype[a];
            args_info[a].name = &empty_sn;
            args_info[a].class_name = &empty_sn;
            args_info[a].hint_string = &empty_string;
            args_info[a].usage = 6;
            args_meta[a] = GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
        }

        GDExtensionClassMethodInfo mi;
        memset(&mi, 0, sizeof mi);
        mi.name = &mname;
        mi.method_userdata = (void *)m;
        mi.call_func = method_call;
        mi.ptrcall_func = method_ptrcall;
        mi.method_flags = GDEXTENSION_METHOD_FLAG_NORMAL;
        mi.has_return_value = (m->rettype != GDEXTENSION_VARIANT_TYPE_NIL);
        mi.return_value_info = &ret_info;
        mi.return_value_metadata = GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
        mi.argument_count = (uint32_t)m->nargs;
        mi.arguments_info = args_info;
        mi.arguments_metadata = args_meta;

        classdb_add_method(api_lib, &sn_class, &mi);
    }
}

static void initialize(void *userdata, GDExtensionInitializationLevel level)
{
    (void)userdata;
    if (level != GDEXTENSION_INITIALIZATION_SCENE) return;
    sn_make(&sn_class, "RunbookWorld");
    sn_make(&sn_parent, "RefCounted");

    GDExtensionClassCreationInfo6 info;
    memset(&info, 0, sizeof info);
    info.is_exposed = 1;
    info.create_instance_func = client_create;
    info.free_instance_func = client_free;

    classdb_register(api_lib, &sn_class, &sn_parent, &info);
    register_methods();
}

static void deinitialize(void *userdata, GDExtensionInitializationLevel level) { (void)userdata; (void)level; }

GDExtensionBool GDE_EXPORT runbook_library_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization)
{
    api_get = p_get_proc_address;
    api_lib = p_library;

    /* NEWEST FIRST, THEN FALL BACK -- the names are versioned and the engine
     * only answers to the ones its own generation knows.
     *
     * NOMINAL paid for this lesson: asking only for the 4.7 names against a
     * 4.6 engine made the library fail to load ENTIRELY, and its gates did not
     * fail -- they passed, for months, measuring a placeholder. The fallback
     * is free: CreationInfo5 is a typedef of Info4, and Info6 differs from it
     * in one field's type between two function pointers with identical
     * signatures, so one filled struct serves whichever registrar answered. */
    GETPROC(classdb_register,    "classdb_register_extension_class6");
    if (!classdb_register) GETPROC(classdb_register, "classdb_register_extension_class5");
    if (!classdb_register) GETPROC(classdb_register, "classdb_register_extension_class4");
    GETPROC(classdb_add_method,  "classdb_register_extension_class_method");
    GETPROC(string_new_utf8,     "string_new_with_utf8_chars");
    GETPROC(string_to_utf8,      "string_to_utf8_chars");
    GETPROC(variant_get_dtor,    "variant_get_ptr_destructor");
    GETPROC(object_set_instance, "object_set_instance");
    GETPROC(classdb_construct,   "classdb_construct_object3");
    if (!classdb_construct) GETPROC(classdb_construct, "classdb_construct_object2");
    GETPROC(stringname_new,      "string_name_new_with_utf8_chars");

    if (!classdb_register || !classdb_add_method || !string_new_utf8 || !classdb_construct) {
        /* Name what is missing. "Something is missing" is not a diagnosis. */
        fprintf(stderr, "runbook: GDExtension entry points missing:%s%s%s%s\n",
                classdb_register   ? "" : " classdb_register_extension_class",
                classdb_add_method ? "" : " classdb_register_extension_class_method",
                string_new_utf8    ? "" : " string_new_with_utf8_chars",
                classdb_construct  ? "" : " classdb_construct_object");
        return 0;
    }

    string_destroy = variant_get_dtor(GDEXTENSION_VARIANT_TYPE_STRING);

    r_initialization->initialize = initialize;
    r_initialization->deinitialize = deinitialize;
    r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
    return 1;
}
