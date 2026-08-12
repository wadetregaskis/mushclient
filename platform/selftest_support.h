// platform/selftest_support.h
//
// Shared helpers for the in-app selftests (the hidden --*-selftest flags).
//
// The selftests run from wxApp::OnInit and exit before InitInstance, which is
// what makes them cheap — no world is opened and nothing of the user's is
// touched — but it also means the App fields InitInstance fills in are still
// empty.  Most of the suite does not care.  PLUGIN LOADING does: the loader
// rejects an empty plugins directory outright ("No plugins directory
// specified", xml/xml_load_world.cpp:2563), so every attempt to load a plugin
// from a selftest failed with eProblemsLoadingPlugin — which read as "the
// harness cannot do plugins" and blocked three real tests.
//
// The headless build has had the same one-line answer since it was written
// (platform/headless_app.cpp:133 sets m_strPluginsDirectory = "."), which is
// why tests/test_plugins.cpp loads plugins perfectly well.  This is that, for
// the GUI selftests.

#pragma once
#ifndef _WIN32

#include <cstdio>
#include <string>
#include <unistd.h>

#include "stdafx.h"
#include "MUSHclient.h"

// Give the app just enough of what InitInstance would have set for a plugin to
// load.  Idempotent, and it only fills fields that are still EMPTY, so a
// selftest that has deliberately set one keeps its value.
//
// The state-files directory is left alone on purpose: CPlugin::SaveState
// returns early when it is empty (plugins.cpp:719), which is the right
// behaviour for a test that does not want state files written, and a test that
// does want them can point it at a scratch directory itself.
inline void SelfTestEnablePluginLoading()
{
    if (App.m_strPluginsDirectory.IsEmpty())
        App.m_strPluginsDirectory = ".";
}

// Write a one-plugin Lua XML file to a scratch path and return it ("" if the
// file could not be written).  `id` must be 24 hex characters — the loader
// checks, and a wrong one fails with a message no caller would expect.
//
// The caller owns the file and should remove() it; the pid and a per-process
// counter keep two selftests (and two runs) from colliding.
inline std::string SelfTestWritePlugin(const char* tag,
                                       const char* id,
                                       const char* name,
                                       const char* scriptBody)
{
    static int seq = 0;
    char path[512];
    snprintf(path, sizeof(path), "/tmp/mc_selftest_plugin_%d_%d_%s.xml",
             (int) getpid(), ++seq, tag ? tag : "p");

    FILE* f = fopen(path, "wb");
    if (!f) return std::string();
    fprintf(f,
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE muclient>\n"
            "<muclient>\n"
            "<plugin name=\"%s\" author=\"selftest\" id=\"%s\" language=\"Lua\" "
            "version=\"1.0\" />\n"
            "<script><![CDATA[\n%s\n]]></script>\n"
            "</muclient>\n",
            name ? name : "SelfTest", id, scriptBody ? scriptBody : "");
    fclose(f);
    return std::string(path);
}

#endif // !_WIN32
