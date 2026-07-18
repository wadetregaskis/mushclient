// errors.cpp — the script-error description table (world.ErrorDesc).
//
// Relocated VERBATIM from scripting/lua_methods.cpp (Campaign C prep) so that
// every build links exactly ONE copy:
//   * GUI builds (Windows / mushapp): lua_methods.cpp declares it extern for
//     MakeIntFlagsTable ("error_desc"), and methods_info.cpp's
//     CMUSHclientDoc::ErrorDesc walks it.
//   * headless (mushcore): the SAME adopted ErrorDesc resolves here — the old
//     windows_stubs.cpp {0,NULL} sentinel (code-as-string fallback) is gone.
// The matching eOK/e* code names live in scripting/errors.h; the Lua-name
// table ("error_code") stays in lua_methods.cpp / lua_world_api_portable.cpp.

#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
#include "stdafx.h"
#include "../MUSHclient.h"
#include "../doc.h"
#else
#include "platform/doc_stub.h"
#endif

int_flags_pair error_descriptions[] =
{

  {  0,      Translate_NoOp ("No error") },
  {  30001,  Translate_NoOp ("The world is already open") },
  {  30002,  Translate_NoOp ("The world is closed, this action cannot be performed") },
  {  30003,  Translate_NoOp ("No name has been specified where one is required") },
  {  30004,  Translate_NoOp ("The sound file could not be played") },
  {  30005,  Translate_NoOp ("The specified trigger name does not exist") },
  {  30006,  Translate_NoOp ("Attempt to add a trigger that already exists") },
  {  30007,  Translate_NoOp ("The trigger \"match\" string cannot be empty") },
  {  30008,  Translate_NoOp ("The name of this object is invalid") },
  {  30009,  Translate_NoOp ("Script name is not in the script file") },
  {  30010,  Translate_NoOp ("The specified alias name does not exist") },
  {  30011,  Translate_NoOp ("Attempt to add a alias that already exists") },
  {  30012,  Translate_NoOp ("The alias \"match\" string cannot be empty") },
  {  30013,  Translate_NoOp ("Unable to open requested file") },
  {  30014,  Translate_NoOp ("Log file was not open") },
  {  30015,  Translate_NoOp ("Log file was already open") },
  {  30016,  Translate_NoOp ("Bad write to log file") },
  {  30017,  Translate_NoOp ("The specified timer name does not exist") },
  {  30018,  Translate_NoOp ("Attempt to add a timer that already exists") },
  {  30019,  Translate_NoOp ("Attempt to delete a variable that does not exist") },
  {  30020,  Translate_NoOp ("Attempt to use SetCommand with a non-empty command window") },
  {  30021,  Translate_NoOp ("Bad regular expression syntax") },
  {  30022,  Translate_NoOp ("Time given to AddTimer is invalid") },
  {  30023,  Translate_NoOp ("Direction given to AddToMapper is invalid") },
  {  30024,  Translate_NoOp ("No items in mapper") },
  {  30025,  Translate_NoOp ("Option name not found") },
  {  30026,  Translate_NoOp ("New value for option is out of range") },
  {  30027,  Translate_NoOp ("Trigger sequence value invalid") },
  {  30028,  Translate_NoOp ("Where to send trigger text to is invalid") },
  {  30029,  Translate_NoOp ("Trigger label not specified/invalid for 'send to variable'") },
  {  30030,  Translate_NoOp ("File name specified for plugin not found") },
  {  30031,  Translate_NoOp ("There was a parsing or other problem loading the plugin") },
  {  30032,  Translate_NoOp ("Plugin is not allowed to set this option") },
  {  30033,  Translate_NoOp ("Plugin is not allowed to get this option") },
  {  30034,  Translate_NoOp ("Requested plugin is not installed") },
  {  30035,  Translate_NoOp ("Only a plugin can do this") },
  {  30036,  Translate_NoOp ("Plugin does not support that subroutine (subroutine not in script)") },
  {  30037,  Translate_NoOp ("Plugin does not support saving state") },
  {  30037,  Translate_NoOp ("Plugin could not save state (eg. no state directory)") },
  {  30039,  Translate_NoOp ("Plugin is currently disabled") },
  {  30040,  Translate_NoOp ("Could not call plugin routine") },
  {  30041,  Translate_NoOp ("Calls to \"Execute\" nested too deeply") },
  {  30042,  Translate_NoOp ("Unable to create socket for chat connection") },
  {  30043,  Translate_NoOp ("Unable to do DNS (domain name) lookup for chat connection") },
  {  30044,  Translate_NoOp ("No chat connections open") },
  {  30045,  Translate_NoOp ("Requested chat person not connected") },
  {  30046,  Translate_NoOp ("General problem with a parameter to a script call") },
  {  30047,  Translate_NoOp ("Already listening for incoming chats") },
  {  30048,  Translate_NoOp ("Chat session with that ID not found") },
  {  30049,  Translate_NoOp ("Already connected to that server/port") },
  {  30050,  Translate_NoOp ("Cannot get (text from the) clipboard") },
  {  30051,  Translate_NoOp ("Cannot open the specified file") },
  {  30052,  Translate_NoOp ("Already transferring a file") },
  {  30053,  Translate_NoOp ("Not transferring a file") },
  {  30054,  Translate_NoOp ("There is not a command of that name") },
  {  30055,  Translate_NoOp ("That array already exists") },
  {  30056,  Translate_NoOp ("That array does not exist") },
  {  30057,  Translate_NoOp ("Values to be imported into array are not in pairs") },
  {  30058,  Translate_NoOp ("Import succeeded, however some values were overwritten") },
  {  30059,  Translate_NoOp ("Import/export delimiter must be a single character, other than backslash") },
  {  30060,  Translate_NoOp ("Array element set, existing value overwritten") },
  {  30061,  Translate_NoOp ("Array key does not exist") },
  {  30062,  Translate_NoOp ("Cannot import because cannot find unused temporary character") },
  {  30063,  Translate_NoOp ("Cannot delete trigger/alias/timer because it is executing a script") },
  {  30064,  Translate_NoOp ("Spell checker is not active") },
  {  30065,  Translate_NoOp ("Cannot create requested font") },                     
  {  30066,  Translate_NoOp ("Invalid settings for pen parameter") },               
  {  30067,  Translate_NoOp ("Bitmap image could not be loaded") },                 
  {  30068,  Translate_NoOp ("Image has not been loaded into window") },            
  {  30069,  Translate_NoOp ("Number of points supplied is incorrect") },           
  {  30070,  Translate_NoOp ("Point is not numeric") },                             
  {  30071,  Translate_NoOp ("Hotspot processing must all be in same plugin") },    
  {  30072,  Translate_NoOp ("Hotspot has not been defined for this window") },     
  {  30073,  Translate_NoOp ("Requested miniwindow does not exist") }, 
  {  30074,  Translate_NoOp ("Invalid settings for brush parameter") },
           
  { 0, nullptr }
};
