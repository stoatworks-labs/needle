#include "Render.h"

#include <algorithm>
#include <vector>

#include "Font.h"
#include "Shaders.h"

namespace needle
{
namespace
{
void SetI( GLuint program, const char* name, int v )
{
	glUniform1i( glGetUniformLocation( program, name ), v );
}
void SetF( GLuint program, const char* name, float v )
{
	glUniform1f( glGetUniformLocation( program, name ), v );
}
void SetI2( GLuint program, const char* name, int a, int b )
{
	glUniform2i( glGetUniformLocation( program, name ), a, b );
}
void SetF2( GLuint program, const char* name, float a, float b )
{
	glUniform2f( glGetUniformLocation( program, name ), a, b );
}
void SetF3( GLuint program, const char* name, const float* v )
{
	glUniform3f( glGetUniformLocation( program, name ), v[ 0 ], v[ 1 ], v[ 2 ] );
}
void SetI4( GLuint program, const char* name, int a, int b, int c, int d )
{
	glUniform4i( glGetUniformLocation( program, name ), a, b, c, d );
}
} // namespace

bool Renderer::InitGL()
{
	if( mReady )
		return true;

	mNote.clear();

	if( !mProgram.Compile( kVertexShader, kFragmentShader ) )
	{
		mNote = "the meter shader would not compile";
		return false;
	}

	glGenVertexArrays( 1, &mVao );

	// The font, as a single-channel texture indexed by ASCII code.
	//
	// The queue is drained first: a plugin that reads a GL error the HOST left
	// behind as its own failure abandons a perfectly good upload. That was
	// gridiron#1 across the fleet.
	while( glGetError() != GL_NO_ERROR )
	{
	}
	const std::vector< uint8_t > pixels = font::Texture();
	glGenTextures( 1, &mFont );
	glBindTexture( GL_TEXTURE_2D, mFont );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, font::kTextureWidth, font::kTextureHeight, 0, GL_RED,
				  GL_UNSIGNED_BYTE, pixels.data() );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	const GLenum err = glGetError();
	if( err != GL_NO_ERROR )
	{
		mNote = "the font texture would not upload (GL error 0x" +
				std::to_string( static_cast< int >( err ) ) + ")";
		DeInitGL();
		return false;
	}

	mReady = true;
	return true;
}

void Renderer::DeInitGL()
{
	if( mFont != 0 )
	{
		glDeleteTextures( 1, &mFont );
		mFont = 0;
	}
	if( mVao != 0 )
	{
		glDeleteVertexArrays( 1, &mVao );
		mVao = 0;
	}
	mProgram.FreeGLResources();
	mReady = false;
}

void Renderer::Draw( const Frame& f, GLuint hostFBO )
{
	if( !mReady || f.width <= 0 || f.height <= 0 )
		return;

	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( 0, 0, f.width, f.height );
	glDisable( GL_DEPTH_TEST );
	// A source owns the layer's buffer, so the composite happens inside the
	// shader and the result is written rather than blended. Blending here would
	// combine with whatever the host left in the buffer, which for a source is
	// the previous frame.
	glDisable( GL_BLEND );

	const GLuint prog = mProgram.GetGLID();
	glUseProgram( prog );

	SetI2( prog, "uSize", f.width, f.height );
	SetI( prog, "uType", f.type );
	SetI( prog, "uCount", f.count );
	SetI( prog, "uStyle", f.style );

	SetF3( prog, "uFace", f.face );
	SetF3( prog, "uNeedleCol", f.needle );
	SetF3( prog, "uBackCol", f.back );
	SetF( prog, "uBackA", f.backAlpha );
	SetF( prog, "uLamp", f.lamp );
	SetF( prog, "uGlass", f.glass );
	SetF( prog, "uWear", f.wear );
	SetF( prog, "uMix", f.mix );

	{
		GLfloat a[ 8 ] = {}, b[ 8 ] = {}, c[ 8 ] = {}, d[ 8 ] = {}, e[ 8 ] = {};
		GLfloat seg[ 80 ] = {};
		GLfloat hold[ 8 ] = {};
		for( int u = 0; u < 2; ++u )
		{
			const Unit& U = f.unit[ u ];
			a[ u * 4 + 0 ] = U.centre[ 0 ];
			a[ u * 4 + 1 ] = U.centre[ 1 ];
			a[ u * 4 + 2 ] = U.rotation;
			a[ u * 4 + 3 ] = U.corner;

			b[ u * 4 + 0 ] = U.halfW;
			b[ u * 4 + 1 ] = U.halfH;
			b[ u * 4 + 2 ] = U.pivot[ 0 ];
			b[ u * 4 + 3 ] = U.pivot[ 1 ];

			c[ u * 4 + 0 ] = U.arcRadius;
			c[ u * 4 + 1 ] = U.arcHalfAngle;
			c[ u * 4 + 2 ] = U.needleLen;
			c[ u * 4 + 3 ] = U.needleWidth;

			d[ u * 4 + 0 ] = U.needleAngle;
			d[ u * 4 + 1 ] = U.redFrom;
			d[ u * 4 + 2 ] = U.eyeRadius;
			d[ u * 4 + 3 ] = U.eyeShadow;

			e[ u * 4 + 0 ] = U.eyeOverlap;
			e[ u * 4 + 1 ] = U.eyeWarm;
			e[ u * 4 + 2 ] = static_cast< float >( U.segLit );
			e[ u * 4 + 3 ] = U.hold[ 2 ] > 0.0f ? 1.0f : 0.0f;

			for( int k = 0; k < standards::kBargraphSteps; ++k )
				for( int j = 0; j < 4; ++j )
					seg[ ( u * standards::kBargraphSteps + k ) * 4 + j ] = U.seg[ k ][ j ];

			for( int j = 0; j < 4; ++j )
				hold[ u * 4 + j ] = U.hold[ j ];
		}
		glUniform4fv( glGetUniformLocation( prog, "uUnitA" ), 2, a );
		glUniform4fv( glGetUniformLocation( prog, "uUnitB" ), 2, b );
		glUniform4fv( glGetUniformLocation( prog, "uUnitC" ), 2, c );
		glUniform4fv( glGetUniformLocation( prog, "uUnitD" ), 2, d );
		glUniform4fv( glGetUniformLocation( prog, "uUnitE" ), 2, e );
		glUniform4fv( glGetUniformLocation( prog, "uSeg" ), 20, seg );
		glUniform4fv( glGetUniformLocation( prog, "uHoldRect" ), 2, hold );
	}

	{
		const int n = std::min( static_cast< int >( f.marks.size() ), Frame::kMaxMarks );
		GLfloat   mark[ Frame::kMaxMarks * 4 ] = {};
		GLint     span[ Frame::kMaxMarks * 2 ] = {};
		for( int m = 0; m < n; ++m )
		{
			mark[ m * 4 + 0 ] = f.marks[ static_cast< size_t >( m ) ].angle;
			mark[ m * 4 + 1 ] = f.marks[ static_cast< size_t >( m ) ].lengthFrac;
			mark[ m * 4 + 2 ] = static_cast< float >( f.marks[ static_cast< size_t >( m ) ].red );
			span[ m * 2 + 0 ] = f.marks[ static_cast< size_t >( m ) ].textOffset;
			span[ m * 2 + 1 ] = f.marks[ static_cast< size_t >( m ) ].textLength;
		}
		SetI( prog, "uMarkCount", n );
		glUniform4fv( glGetUniformLocation( prog, "uMark" ), Frame::kMaxMarks, mark );
		glUniform2iv( glGetUniformLocation( prog, "uMarkText" ), Frame::kMaxMarks, span );

		GLint text[ Frame::kMaxText ];
		for( int i = 0; i < Frame::kMaxText; ++i )
			text[ i ] = ( i < static_cast< int >( f.text.size() ) ) ? f.text[ static_cast< size_t >( i ) ] : 32;
		glUniform1iv( glGetUniformLocation( prog, "uText" ), Frame::kMaxText, text );
	}

	SetI4( prog, "uLegend", f.legendOffset, f.legendLength, std::max( 1, f.legendScale ),
		   std::max( 1, f.markTextScale ) );
	SetF2( prog, "uLegendPos", f.legendPos[ 0 ], f.legendPos[ 1 ] );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mFont );
	SetI( prog, "uFont", 0 );

	glBindVertexArray( mVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );

	// ---- hand the context back ---------------------------------------------
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glUseProgram( 0 );
}

} // namespace needle
