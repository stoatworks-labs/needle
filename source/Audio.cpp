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

	return static_cast< float >( std::sqrt( total ) );
}

} // namespace needle::audio
