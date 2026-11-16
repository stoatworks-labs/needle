#include "VfdRender.h"

#include "Shaders.h"
#include "VfdShaders.h"

namespace needle::vfd
{
bool Renderer::Ensure()
{
	if( mReady )
		return true;
	if( mTried )
		return false;
	mTried = true;
	if( !mProgram.Compile( kVertexShader, kVfdFragmentShader ) )
	{
		mNote = "the VFD shader would not compile";
		return false;
	}
	glGenVertexArrays( 1, &mVao );
	mReady = true;
	return true;
}

void Renderer::DeInitGL()
{
	if( mVao != 0 )
	{
		glDeleteVertexArrays( 1, &mVao );
		mVao = 0;
	}
	mProgram.FreeGLResources();
	mReady = false;
	mTried = false;
}

void Renderer::Draw( const Picture& pic, GLuint hostFBO )
{
	if( !mReady || pic.width <= 0 || pic.height <= 0 )
		return;

	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( 0, 0, pic.width, pic.height );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );// a source writes its layer; the composite is in the shader

	const GLuint prog = mProgram.GetGLID();
	glUseProgram( prog );
	auto at = [ prog ]( const char* name ) { return glGetUniformLocation( prog, name ); };

	glUniform2i( at( "uSize" ), pic.width, pic.height );
	glUniform4f( at( "uXform" ), pic.centre[ 0 ], pic.centre[ 1 ], pic.rotation, pic.corner );
	glUniform2f( at( "uHalf" ), pic.halfW, pic.halfH );
	glUniform4f( at( "uField" ), pic.field[ 0 ], pic.field[ 1 ], pic.field[ 2 ], pic.field[ 3 ] );
	glUniform2f( at( "uSeg" ), pic.seg[ 0 ], pic.seg[ 1 ] );
	glUniform2i( at( "uGrid" ), pic.cols, pic.rows );
	glUniform1fv( at( "uLit" ), kMaxColumns, pic.lit );
	glUniform1fv( at( "uDot" ), kMaxColumns, pic.dot );
	glUniform3f( at( "uPhosphor" ), pic.phosphor[ 0 ], pic.phosphor[ 1 ], pic.phosphor[ 2 ] );
	glUniform4f( at( "uWireY" ), pic.wireY[ 0 ], pic.wireY[ 1 ], pic.wireY[ 2 ], pic.wireY[ 3 ] );
	glUniform3f( at( "uWire" ), pic.wireW, pic.wireX0, pic.wireX1 );
	glUniform4f( at( "uScan" ), static_cast< float >( pic.camera ), pic.phase0, pic.rowStep, pic.window );
	glUniform3f( at( "uSlot" ), pic.slot, pic.blank, pic.pulse );
	glUniform4f( at( "uLampA" ), pic.lamp[ 0 ], pic.lamp[ 1 ], pic.lamp[ 2 ], pic.lamp[ 3 ] );
	glUniform3f( at( "uLampCol" ), pic.lampColour[ 0 ], pic.lampColour[ 1 ], pic.lampColour[ 2 ] );
	glUniform3f( at( "uBackCol" ), pic.back[ 0 ], pic.back[ 1 ], pic.back[ 2 ] );
	glUniform1f( at( "uBackA" ), pic.backAlpha );
	glUniform1f( at( "uMix" ), pic.mix );

	glBindVertexArray( mVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );

	glBindVertexArray( 0 );
	glUseProgram( 0 );
}
} // namespace needle::vfd
