static const uint MAX_BOUNCES = 4;
static const float PI = 3.1415926535;

cbuffer SceneCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4 cam_position;

    float4 spheres[128];
    float4 colors[128];
    uint4 materials[128];
    float4 ground_color;
    float ground_y;

    uint frame_index;
    uint num_spheres;
    uint rays_per_pixel;

    uint width;
    uint height;
    uint _pad0;
    uint _pad1;
};

struct [raypayload] RayPayload {
    float4 color      : read(caller) : write(caller, miss);
    float3 hit_pos    : read(caller) : write(caller, closesthit);
    float3 hit_normal : read(caller) : write(caller, closesthit);
    uint hit_index    : read(caller) : write(caller, closesthit);
    uint did_hit      : read(caller) : write(caller, miss, closesthit);
};

struct ProceduralPrimitiveAttributes {
    float3 normal;
    uint sphere_index;
};

// --- RNG utilities ---

uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x45d9f3bu;
    x ^= x >> 16;
    x *= 0x45d9f3bu;
    x ^= x >> 16;
    return x;
}

float rand_float(inout uint seed) {
    seed = hash(seed);
    return float(seed) / 4294967295.0f;
}

float rand_float(inout uint seed, float min, float max) {
    return min + (max - min) * rand_float(seed);
}

float3 rand_unit_sphere(inout uint seed)
{
    float phi = 2.0 * PI * rand_float(seed);
    float cosTheta = 2.0 * rand_float(seed) - 1.0;
    float u = rand_float(seed);

    float theta = acos(cosTheta);
    float r = pow(u, 1.0 / 3.0);

    float x = r * sin(theta) * cos(phi);
    float y = r * sin(theta) * sin(phi);
    float z = r * cos(theta);

    return float3(x, y, z);
}

float3 rand_unit_disk(inout uint seed) {
    float sin_t = 2.0 * rand_float(seed) - 1.0;
    float cos_t = 2.0 * rand_float(seed) - 1.0;

    float x = cos(cos_t);
    float y = cos(sin_t);

    return float3(x, y, 0);
}

float3 rand_unit_vector(inout uint seed) {
    return normalize(rand_unit_sphere(seed));
}



float3 random_cosine_hemisphere(inout uint seed, float3 normal) {
    float u1 = rand_float(seed);
    float u2 = rand_float(seed);

    float r = sqrt(u1);
    float theta = 2.0f * PI * u2;

    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(1.0f - u1);

    // Build tangent frame from normal
    float3 up = abs(normal.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent   = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    return normalize(tangent * x + bitangent * y + normal * z);
}

// --- Shaders ---

[shader("raygeneration")]
void raygen_main() {
    RWTexture2D<float4> output = ResourceDescriptorHeap[frame_index];
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[2 + frame_index];

    uint2 pixel = DispatchRaysIndex().xy;
    //uint seed = hash(pixel.x * 1973 + pixel.y * 9277 + frame_index * 26699);
    uint seed = hash(pixel.x * 1973 + pixel.y * 9277 * 26699);

    float3 accumulated_color = float3(0, 0, 0);

    for (uint sample_i = 0; sample_i < rays_per_pixel; sample_i++) {
        // Jitter the sub-pixel offset for anti-aliasing
        float jitter_x = rand_float(seed);
        float jitter_y = rand_float(seed);

        // Compute NDC coordinates [-1, 1] with jitter
        float2 ndc = float2(
            (pixel.x + jitter_x) / (float)width  *  2.0f - 1.0f,
            (pixel.y + jitter_y) / (float)height * -2.0f + 1.0f
        );

        // Unproject near and far points through inverse view-projection
        float4 world_near = mul(float4(ndc, 0.0f, 1.0f), inv_view_proj);
        float4 world_far  = mul(float4(ndc, 1.0f, 1.0f), inv_view_proj);
        world_near /= world_near.w;
        world_far  /= world_far.w;

        float3 origin    = world_near.xyz;
        float3 direction = normalize(world_far.xyz - world_near.xyz);

        // Path tracing loop — throughput tracks how much light each bounce lets through
        float3 throughput = float3(1, 1, 1);
        float3 sample_color = float3(0, 0, 0);

        for (uint bounce = 0; bounce <= MAX_BOUNCES; bounce++) {
            RayDesc ray;
            ray.Origin    = origin;
            ray.Direction = direction;
            ray.TMin      = 0.001f;
            ray.TMax      = 10000.0f;

            RayPayload payload;
            payload.color      = float4(0, 0, 0, 1);
            payload.hit_pos    = float3(0, 0, 0);
            payload.hit_normal = float3(0, 0, 0);
            payload.hit_index  = 0;
            payload.did_hit    = 0;

            TraceRay(tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

            // Analytical ground plane intersection (infinite XZ plane at y = ground_y)
            bool hit_ground = false;
            float ground_t = -1.0f;
            if (abs(direction.y) > 1e-6f) {
                ground_t = (ground_y - origin.y) / direction.y;
                if (ground_t > ray.TMin) {
                    // Ground is in front of the ray — check if it's closer than geometry
                    float geo_t = payload.did_hit ? length(payload.hit_pos - origin) : ray.TMax;
                    if (ground_t < geo_t) {
                        hit_ground = true;
                    }
                }
            }

            if (hit_ground) {
                // Ground hit — diffuse bounce off the ground plane
                throughput *= ground_color.rgb;
                origin = origin + ground_t * direction;
                direction = random_cosine_hemisphere(seed, float3(0, 1, 0));
            } else if (!payload.did_hit) {
                // Ray escaped to sky — accumulate sky light modulated by throughput
                sample_color = throughput * payload.color.rgb;
                break;
            } else {
                // Geometry hit — modulate throughput by surface albedo
                float3 albedo = colors[payload.hit_index].rgb;
                throughput *= albedo;

                // Set up next bounce
                origin = payload.hit_pos;

                uint mat_type = materials[payload.hit_index].x;
                if (mat_type == 1) {
                    // Mirror: reflect ray around surface normal
                    direction = reflect(direction, payload.hit_normal);
                } else {
                    // Diffuse: scatter in random hemisphere direction
                    direction = random_cosine_hemisphere(seed, payload.hit_normal);
                }
            }
        }

        accumulated_color += sample_color;
    }

    // Average all samples
    output[pixel] = float4(accumulated_color / (float)rays_per_pixel, 1.0f);
}

[shader("miss")]
void miss_background(inout RayPayload payload) {
    float3 dir = normalize(WorldRayDirection());
    float a = 0.5f * (dir.y + 1.0f);
    payload.color  = float4((1.0f - a) * float3(1, 1, 1) + a * float3(0.5f, 0.7f, 1.0f), 1.0f);
    payload.did_hit = 0;
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
    payload.hit_pos    = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    payload.hit_normal = normalize(attributes.normal);
    payload.hit_index  = attributes.sphere_index;
    payload.did_hit    = 1;
}
