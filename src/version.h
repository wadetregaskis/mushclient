// Version number for this release. Used in various places.

// Used for detecting version changes
#define THISVERSION 600
// Used in the resource file
#define VERSION_16WORDS 6,0,0,0
// Used to display the version number.
//
// Keep upstream's "<major>.<two-digit minor>" shape ("5.07" <-> THISVERSION 507),
// never a dotted triple.  This is what world.Version() and GetInfo (72) return,
// and plugins read it with Lua's tonumber -- including the bundled
// Version_Check.xml and Quow's Cow Bar, which compares it against 5.07 in its
// main chunk.  tonumber ("6.0.0") is nil, so a triple stops such plugins from
// loading at all.
#define VERSION_STRING "6.00"

// Note that the VERSION_STRING will have "-pre" appended by a pre-release build.

// remember to edit date and version in readme.txt file
//                  ^^^^^^^^^^^^^^^^
