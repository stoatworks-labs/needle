#pragma once

#include <string>

#include "Vfd.h"

#include <FFGLSDK.h>

namespace needle::vfd
{
/**
	The VFD's draw: its own program, one triangle, into the host's FBO.
	Compiled the first time a VFD frame is drawn, never in InitGL, so a host
	that never selects the VFD never compiles it. What it binds it unbinds.
*/
class Renderer
{
public:
	bool Ensure();///< compile on first use; false (with Note()) if it cannot
	void DeInitGL();
	bool Ready() const { return mReady; }
	void Draw( const Picture& pic, GLuint hostFBO );
	const std::string& Note() const { return mNote; }

private:
	ffglex::FFGLShader mProgram;
	GLuint             mVao   = 0;
	bool               mReady = false;
	bool               mTried = false;
	std::string        mNote;
};
} // namespace needle::vfd
