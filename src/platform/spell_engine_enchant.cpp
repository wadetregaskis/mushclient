// spell_engine_enchant.cpp -- Linux's spell checker behind MUSHclient's:
// Enchant, the library GNOME's and others' spell checking goes through, which
// in turn uses Hunspell (or Nuspell, Aspell...) and the distribution's
// dictionaries.  See spell_engine.h.
//
// Loaded with dlopen rather than linked, so that neither building MUSHclient
// nor running it needs Enchant: without libenchant-2, or without a dictionary
// for the language, the built-in word lists are used.  The language is the
// one $LC_ALL, $LC_MESSAGES or $LANG names ("en_AU.UTF-8" -> "en_AU"), then
// that without its region, then en_US.  "Add to System" is Enchant's personal
// word list for that language (~/.config/enchant/), which other programs using
// Enchant share.  Words go in and out as UTF-8.

#include "spell_engine.h"

#include <dlfcn.h>
#include <cstdlib>
#include <sys/types.h>

namespace {

struct EnchantBroker;
struct EnchantDict;

struct EnchantApi
{
    EnchantBroker * (*broker_init) ();
    void            (*broker_free) (EnchantBroker *);
    int             (*broker_dict_exists) (EnchantBroker *, const char *);
    EnchantDict *   (*broker_request_dict) (EnchantBroker *, const char *);
    void            (*broker_free_dict) (EnchantBroker *, EnchantDict *);
    int             (*dict_check) (EnchantDict *, const char *, ssize_t);
    char **         (*dict_suggest) (EnchantDict *, const char *, ssize_t, size_t *);
    void            (*dict_free_string_list) (EnchantDict *, char **);
    void            (*dict_add) (EnchantDict *, const char *, ssize_t);
};

template <typename T>
bool Resolve (void * lib, const char * name, T & fn)
{
    fn = reinterpret_cast<T> (dlsym (lib, name));
    return fn != nullptr;
}

class CEnchantSpellEngine : public ISpellEngine
{
public:
    CEnchantSpellEngine (const EnchantApi & api, EnchantBroker * broker,
                         EnchantDict * dict, const std::string & language)
        : m_api (api), m_broker (broker), m_dict (dict), m_language (language) {}
    ~CEnchantSpellEngine () override
    {
        m_api.broker_free_dict (m_broker, m_dict);
        m_api.broker_free (m_broker);
        // the library stays loaded: harmless, and unloading it is not
    }

    std::string Name () override
    {
        return "Enchant (" + m_language + ")";
    }

    bool Check (const std::string & word) override
    {
        // 0 is right, more is wrong, less is an error -- not worth a nag
        return word.empty () || m_api.dict_check (m_dict, word.c_str (), (ssize_t) word.size ()) <= 0;
    }

    std::vector<std::string> Suggest (const std::string & word) override
    {
        std::vector<std::string> suggestions;
        size_t n = 0;
        char ** list = m_api.dict_suggest (m_dict, word.c_str (), (ssize_t) word.size (), &n);
        if (!list)
            return suggestions;
        for (size_t i = 0; i < n; i++)
            if (list [i])
                suggestions.push_back (list [i]);
        m_api.dict_free_string_list (m_dict, list);
        return suggestions;
    }

    bool AddWord (const std::string & word) override
    {
        m_api.dict_add (m_dict, word.c_str (), (ssize_t) word.size ());
        return true;
    }

private:
    EnchantApi      m_api;
    EnchantBroker * m_broker;
    EnchantDict *   m_dict;
    std::string     m_language;
};

// "en_AU.UTF-8@euro" -> "en_AU"
std::string LanguageOf (const char * value)
{
    std::string s = value ? value : "";
    const size_t cut = s.find_first_of (".@");
    if (cut != std::string::npos)
        s.resize (cut);
    return s == "C" || s == "POSIX" ? std::string () : s;
}

}   // namespace

ISpellEngine * CreatePlatformSpellEngine ()
{
    void * lib = dlopen ("libenchant-2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen ("libenchant-2.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return nullptr;

    EnchantApi api;
    if (!Resolve (lib, "enchant_broker_init",           api.broker_init) ||
        !Resolve (lib, "enchant_broker_free",           api.broker_free) ||
        !Resolve (lib, "enchant_broker_dict_exists",    api.broker_dict_exists) ||
        !Resolve (lib, "enchant_broker_request_dict",   api.broker_request_dict) ||
        !Resolve (lib, "enchant_broker_free_dict",      api.broker_free_dict) ||
        !Resolve (lib, "enchant_dict_check",            api.dict_check) ||
        !Resolve (lib, "enchant_dict_suggest",          api.dict_suggest) ||
        !Resolve (lib, "enchant_dict_free_string_list", api.dict_free_string_list) ||
        !Resolve (lib, "enchant_dict_add",              api.dict_add))
        return nullptr;

    EnchantBroker * broker = api.broker_init ();
    if (!broker)
        return nullptr;

    // The locale as POSIX decides it: the first of these that is set at all
    // wins, even "C" (which names no language).  $LANGUAGE, a list of
    // languages in order of preference, comes first where the locale is not
    // "C" -- as gettext has it.
    std::string locale;
    for (const char * var : { "LC_ALL", "LC_MESSAGES", "LANG" })
    {
        const char * value = getenv (var);
        if (value && *value)
        {
            locale = value;
            break;
        }
    }
    std::vector<std::string> wanted;
    const std::string language = LanguageOf (locale.c_str ());
    if (!language.empty ())
        if (const char * list = getenv ("LANGUAGE"))
        {
            std::string item;
            for (const char * p = list; ; ++p)
                if (*p == ':' || *p == 0)
                {
                    if (!LanguageOf (item.c_str ()).empty ())
                        wanted.push_back (LanguageOf (item.c_str ()));
                    item.clear ();
                    if (!*p)
                        break;
                }
                else
                    item += *p;
        }
    wanted.push_back (language);
    std::vector<std::string> candidates;
    for (const std::string & w : wanted)
    {
        if (w.empty ())
            continue;
        candidates.push_back (w);
        const size_t underscore = w.find ('_');
        if (underscore != std::string::npos)
            candidates.push_back (w.substr (0, underscore));
    }
    candidates.push_back ("en_US");

    for (const std::string & language : candidates)
        if (api.broker_dict_exists (broker, language.c_str ()))
            if (EnchantDict * dict = api.broker_request_dict (broker, language.c_str ()))
                return new CEnchantSpellEngine (api, broker, dict, language);

    api.broker_free (broker);
    return nullptr;
}
