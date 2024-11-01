#ifndef MOER_MATH_HLSL
#define MOER_MATH_HLSL

#define PI 3.1415926535897932384626433832795
namespace Math{

    #define _Pi(x) radians( 180.0 * x )

    float Pi( float x )
    {
        return _Pi( x );
    }

    float2 Pi( float2 x )
    {
        return _Pi( x );
    }

    float3 Pi( float3 x )
    {
        return _Pi( x );
    }

    float4 Pi( float4 x )
    {
        return _Pi( x );
    }

    #define _RadToDeg( x ) ( x * 180.0 / Pi( 1.0 ) )

    float RadToDeg( float x )
    {
        return _RadToDeg( x );
    }

    float2 RadToDeg( float2 x )
    {
        return _RadToDeg( x );
    }

    float3 RadToDeg( float3 x )
    {
        return _RadToDeg( x );
    }

    float4 RadToDeg( float4 x )
    {
        return _RadToDeg( x );
    }

    #define _DegToRad( x ) ( x * Pi( 1.0 ) / 180.0 )

    float DegToRad( float x )
    {
        return _DegToRad( x );
    }

    float2 DegToRad( float2 x )
    {
        return _DegToRad( x );
    }

    float3 DegToRad( float3 x )
    {
        return _DegToRad( x );
    }

    float4 DegToRad( float4 x )
    {
        return _DegToRad( x );
    }
}
#endif