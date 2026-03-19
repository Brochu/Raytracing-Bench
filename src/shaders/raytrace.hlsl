cbuffer SceneCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4 cam_position;

    uint width;
    uint height;

    uint num_spheres;
    uint frame_index;
    float4 spheres[128];
    float4 colors[128];
    int types[128];
};

struct [raypayload] RayPayload {
    float4 color : read(caller) : write(caller, miss, closesthit);
    //uint recursion_depth : read() : write();
};

struct ProceduralPrimitiveAttributes {
    float3 normal;
    uint sphere_index;
};

[shader("raygeneration")]
void raygen_main() {
    RWTexture2D<float4> output = ResourceDescriptorHeap[frame_index];
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[2 + frame_index];

    uint2 pixel = DispatchRaysIndex().xy;

    // Compute NDC coordinates [-1, 1]
    float2 ndc = float2(
        (pixel.x + 0.5f) / (float)width  *  2.0f - 1.0f,
        (pixel.y + 0.5f) / (float)height * -2.0f + 1.0f
    );

    // Unproject near and far points through inverse view-projection
    float4 world_near = mul(float4(ndc, 0.0f, 1.0f), inv_view_proj);
    float4 world_far  = mul(float4(ndc, 1.0f, 1.0f), inv_view_proj);
    world_near /= world_near.w;
    world_far  /= world_far.w;

    float3 origin    = world_near.xyz;
    float3 direction = normalize(world_far.xyz - world_near.xyz);

    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = 0.001f;
    ray.TMax      = 10000.0f;

    RayPayload payload;
    payload.color = float4(0, 0, 0, 1);
    //payload.recursion_depth = 0;

    TraceRay(tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    output[pixel] = payload.color;
}

[shader("miss")]
void miss_background(inout RayPayload payload) {
    float t = saturate(WorldRayDirection().y * 0.5f + 0.5f);
    float3 pale_blue = float3(0.4f, 0.6f, 1.0f);

    payload.color = float4(lerp(pale_blue, float3(1, 1, 1), t), 1.0f);
}

[shader("intersection")]
void sphere_intersection() {
    float3 origin = ObjectRayOrigin();
    float3 dir    = ObjectRayDirection();

    float closest_t = RayTCurrent();
    int closest_idx = -1;
    float3 closest_normal = float3(0, 0, 0);

    for (uint i = 0; i < num_spheres; i++) {
        float3 center = spheres[i].xyz;
        float radius  = spheres[i].w;

        float3 oc = origin - center;
        float a = dot(dir, dir);
        float b = dot(oc, dir);
        float c = dot(oc, oc) - radius * radius;
        float discriminant = b * b - a * c;

        if (discriminant < 0)
            continue;

        float sqrt_disc = sqrt(discriminant);
        float t = (-b - sqrt_disc) / a;

        if (t < RayTMin() || t > closest_t) {
            t = (-b + sqrt_disc) / a;
            if (t < RayTMin() || t > closest_t)
                continue;
        }

        closest_t = t;
        closest_idx = (int)i;
        closest_normal = (origin + t * dir - center) / radius;
    }

    if (closest_idx >= 0) {
        ProceduralPrimitiveAttributes attr;
        attr.normal = closest_normal;
        attr.sphere_index = (uint)closest_idx;
        ReportHit(closest_t, 0, attr);
    }
}

[shader("closesthit")]
void closest_main(inout RayPayload payload, in ProceduralPrimitiveAttributes attributes) {
    float3 view_dir = normalize(cam_position.xyz - (WorldRayOrigin() + RayTCurrent() * WorldRayDirection()));
    float ndotv = saturate(dot(attributes.normal, view_dir));
    payload.color = float4(colors[attributes.sphere_index].rgb * ndotv, 1.0f);
}
