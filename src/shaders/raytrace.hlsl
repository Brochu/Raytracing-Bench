struct [raypayload] RayPayload {
    float4 color : read() : write();
    uint recursion_depth : read() : write();
};

struct ProceduralPrimitiveAttributes {
    float3 normal;
};

[shader("raygeneration")]
void raygen_main() {
}

[shader("miss")]
void miss_background(inout RayPayload payload) {
}

[shader("intersection")]
void sphere_intersection() {
}

[shader("closesthit")]
void closest_main(inout RayPayload payload, in ProceduralPrimitiveAttributes attributes) {
}
