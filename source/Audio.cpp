#include "Audio.h"

#include <algorithm>
#include <cmath>

namespace needle::audio
{

float LevelFromSpectrum( const float* bins, int count, bool power )
{
	if( bins == nullptr || count <= 0 )
		return 0.0f;

	double total = 0.0;
	for( int i = 0; i < count; ++i )
	{
		// A host is entitled to hand back a negative or a NaN in a buffer it
		// has not filled yet. A NaN here would propagate all the way to the
		// needle angle and the meter would simply vanish, with nothing in the
		// log to say why -- so it is stopped at the door.
		const float b = bins[ i ];
		if( !( b > 0.0f ) )
			continue;
		total += power ? static_cast< double >( b )
					   : static_cast< double >( b ) * static_cast< double >( b );
	}

	// The MEAN over the bins, not the sum: the FFGL SDK's own volume, which
	// `ndtest --defaults` holds this to. The sum read 18.06 dB hot on a real
	// Resolume rig -- see "The level" in Audio.h.
	return static_cast< float >( std::sqrt( total / static_cast< double >( count ) ) );
}

} // namespace needle::audio
