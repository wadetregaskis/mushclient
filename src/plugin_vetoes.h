// plugin_vetoes.h - who has stopped which commands.
//
// A plugin can stop a command with OnPluginSend or OnPluginCommand (return
// false), or OnPluginCommandEntered (return a tab or a carriage return).  That
// is a legitimate thing to do, so this is only bookkeeping: each time, a Trace
// message, and a per-plugin count that GetPluginInfo (selector 26) and
// Debug "plugins" show.
//
// The one exception is PluginSendVetoIgnored: see
// docs/QUOW-HIDDEN-COMMAND-DROP.md.  The bodies are in plugins.cpp.

#pragma once

class CPlugin;

// Is this a plugin whose OnPluginSend refusals the client does not honour?
bool PluginSendVetoIgnored (const CString & strPluginID, const double dVersion);

// Commands this plugin stopped, since it was loaded.
long PluginCommandsStopped (const CPlugin * pPlugin);

// OnPluginSend refusals from this plugin that the client ignored, since it was loaded.
long PluginVetoesIgnored (const CPlugin * pPlugin);
