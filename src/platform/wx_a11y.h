// wx_a11y.h -- what the wx frontend needs that wx does not have: wxAccessible is MSW only, so a
// control's name for VoiceOver and Orca is native code, in wx_osx_speech.mm and linux_speech.cpp.

#pragma once

class wxWindow;

void McSetAccessibleName (wxWindow * window, const char * utf8Name);
