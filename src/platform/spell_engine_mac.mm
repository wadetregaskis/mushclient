// spell_engine_mac.mm -- macOS's spell checker (NSSpellChecker) behind
// MUSHclient's.  See spell_engine.h.
//
// Obj-C++, and including nothing of MUSHclient's but spell_engine.h, for the
// reason wx_osx_text.mm gives: AppKit's types must not meet the MFC headers.
//
// Language nil throughout: NSSpellChecker then uses the languages the user
// has chosen in System Settings > Keyboard > Text Input -- automatically by
// language, if that is how they have it set.  Words go in and out as UTF-8.
// "Add to System" is learnWord:, the dictionary every Mac app shares; the
// spell document tag keeps this program's "ignore" state to itself.

#import <AppKit/AppKit.h>

#include "spell_engine.h"

namespace {

NSString * ToNS (const std::string & s)
{
    return [NSString stringWithUTF8String: s.c_str ()];
}

class CMacSpellEngine : public ISpellEngine
{
public:
    CMacSpellEngine () : m_tag ([NSSpellChecker uniqueSpellDocumentTag]) {}
    ~CMacSpellEngine () override
    {
        [[NSSpellChecker sharedSpellChecker] closeSpellDocumentWithTag: m_tag];
    }

    std::string Name () override
    {
        @autoreleasepool {
            NSString * language = [[NSSpellChecker sharedSpellChecker] language];
            return std::string ("macOS (") + (language ? [language UTF8String] : "") + ")";
        }
    }

    bool Check (const std::string & word) override
    {
        @autoreleasepool {
            NSString * s = ToNS (word);
            if (!s || [s length] == 0)
                return true;
            const NSRange wrong = [[NSSpellChecker sharedSpellChecker]
                checkSpellingOfString: s
                           startingAt: 0
                             language: nil
                                 wrap: NO
               inSpellDocumentWithTag: m_tag
                            wordCount: NULL];
            return wrong.location == NSNotFound;
        }
    }

    std::vector<std::string> Suggest (const std::string & word) override
    {
        std::vector<std::string> suggestions;
        @autoreleasepool {
            NSString * s = ToNS (word);
            if (!s)
                return suggestions;
            NSArray<NSString *> * guesses = [[NSSpellChecker sharedSpellChecker]
                guessesForWordRange: NSMakeRange (0, [s length])
                           inString: s
                           language: nil
             inSpellDocumentWithTag: m_tag];
            for (NSString * guess in guesses)
                suggestions.push_back ([guess UTF8String]);
        }
        return suggestions;
    }

    bool AddWord (const std::string & word) override
    {
        @autoreleasepool {
            NSString * s = ToNS (word);
            if (!s)
                return false;
            [[NSSpellChecker sharedSpellChecker] learnWord: s];
            return true;
        }
    }

private:
    NSInteger m_tag;
};

}   // namespace

ISpellEngine * CreatePlatformSpellEngine ()
{
    // No dictionaries at all and every word would pass: the built-in lists
    // are better than that.
    @autoreleasepool
    {
        if ([[[NSSpellChecker sharedSpellChecker] availableLanguages] count] == 0)
            return nullptr;
    }
    return new CMacSpellEngine ();
}
