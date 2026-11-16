// AIR.4E.2 divergence diagnostic: one thread per window slot writes both pieces' outflow (AirDivergence.hlsli) as a float4,
// (face, collocated) of piece 0 then of piece 1, into a raw buffer in the velocity buffers' slot order. Reads the published field
// (shader-read state) and writes only its own buffer: the simulation is unchanged. Recorded by EditorAirPass while the Editor's
// divergence view is shown.
//
// AIR.4E.4: with a face source (a float2-per-slot buffer of the pressure solver, faceSourceScale times it), the face value comes from
// it instead: the projected step's predicted face divergence (-residual) or its outflow before the projection (-rhs). The collocated
// value is always the stored velocity's.

#include "AirDivergence.hlsli"

cbuffer AirDivergenceConstants : register(b0)
{
    AirFieldBinding field;
    uint apertureIndex;
    uint pressureIndex;
    // Writable view of the output (16 bytes per slot).
    uint outputIndex;
    // AIR.4E.4: the face source's read-only view, or AirNoPressure; and its scale.
    uint faceSourceIndex;
    float3 ambientVelocity;
    float faceSourceScale;
};

[numthreads(8, 8, 8)]
void CSDivergence(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    AirDivergenceInputs inputs;
    inputs.field = field;
    inputs.apertureIndex = apertureIndex;
    inputs.pressureIndex = pressureIndex;
    inputs.ambient = ambientVelocity;
    inputs.buoyancy = AirNoBuoyancy();

    const int3 cell = AirCellOfSlot(field, slot);
    AirPieceDivergence divergence = AirCellDivergence(inputs, cell);

    if(faceSourceIndex != AirNoPressure)
    {
        ByteAddressBuffer source = ResourceDescriptorHeap[NonUniformResourceIndex(faceSourceIndex)];
        const AirPieces pieces = AirPiecesOf(field, cell);
        const float2 values = asfloat(source.Load2(AirDivergenceSlotIndex(field, cell) * 8)) * faceSourceScale;
        divergence.piece0.x = pieces.piece0 != 0 ? values.x : 0.0;
        divergence.piece1.x = pieces.piece1 != 0 ? values.y : 0.0;
    }

    RWByteAddressBuffer output = ResourceDescriptorHeap[NonUniformResourceIndex(outputIndex)];
    output.Store4(AirVelocityAddress(field, slot), asuint(float4(divergence.piece0, divergence.piece1)));
}
