// The material nodes (material.h): copied and adapted from the Tlender's nodes.c (Blender's sockets, order, defaults and
// enums), the subset the Real Engine compiles. Keep the order of each node's sockets and properties as the Tlender's:
// its files link and store values by socket index.
#include "material.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static MatNodeDef g_defs[NODE_TYPE_COUNT + 1]; // (+ the Reroute)
static int g_order[NODE_TYPE_COUNT + 1], g_count;
static bool g_ready;

#define BIG 1e9f

static MatNodeDef* def_begin(int type, const char* idname, const char* name, const char* label, int cat, float width) {
    MatNodeDef* d = &g_defs[type];
    memset(d, 0, sizeof *d);
    d->type = type;
    d->idname = idname;
    d->name = name;
    d->label = label;
    d->category = cat;
    d->width = width;
    g_order[g_count++] = type;
    return d;
}
static void add_in(MatNodeDef* d, const char* name, const char* label, int type, float a, float b, float c, float lo, float hi, int flags) {
    MatSockDef* s = &d->in[d->nin++];
    s->name = name;
    s->label = label;
    s->type = (uint8_t)type;
    s->flags = (uint16_t)flags;
    s->def[0] = a, s->def[1] = b, s->def[2] = c, s->def[3] = 1.0f;
    s->min = lo, s->max = hi;
}
static void in_float(MatNodeDef* d, const char* n, const char* l, float v, float lo, float hi, int f) { add_in(d, n, l, MSOCK_FLOAT, v, 0, 0, lo, hi, f); }
static void in_fac(MatNodeDef* d, const char* n, const char* l, float v) { add_in(d, n, l, MSOCK_FLOAT, v, 0, 0, 0, 1, MSF_FACTOR); }
static void in_color(MatNodeDef* d, const char* n, const char* l, float r, float g, float b) { add_in(d, n, l, MSOCK_COLOR, r, g, b, 0, 1, 0); }
static void in_vector(MatNodeDef* d, const char* n, const char* l, float x, float y, float z, int f) { add_in(d, n, l, MSOCK_VECTOR, x, y, z, -BIG, BIG, f); }
static void in_normal(MatNodeDef* d, const char* n, const char* l) { add_in(d, n, l, MSOCK_VECTOR, 0, 0, 0, -1, 1, MSF_HIDE_VALUE | MSF_IMPLICIT_NORMAL); }
static void in_shader(MatNodeDef* d, const char* n, const char* l) { add_in(d, n, l, MSOCK_SHADER, 0, 0, 0, 0, 0, MSF_HIDE_VALUE); }
static void out(MatNodeDef* d, const char* n, const char* l, int type) {
    MatSockDef* s = &d->out[d->nout++];
    s->name = n;
    s->label = l;
    s->type = (uint8_t)type;
}
static void prop(MatNodeDef* d, const char* label, const char* const* items, int count, int def) {
    MatPropDef* p = &d->props[d->nprops++];
    p->label = label;
    p->items = items;
    p->count = count;
    p->def = def;
}
static void prop_bool(MatNodeDef* d, const char* label, int def) { prop(d, label, NULL, 2, def); }

static const char* const DISTRIBUTIONS[] = { "Multiscatter GGX", "GGX" };
static const char* const SSS_METHODS[] = { "Random Walk", "Random Walk (Skin)", "Christensen-Burley" };
static const char* const GRADIENTS[] = { "Linear", "Quadrático", "Suavizado", "Diagonal", "Esférico", "Esfera quadrática", "Radial" };
static const char* const WAVE_TYPES[] = { "Faixas", "Anéis" };
static const char* const WET_MASKS[] = { "Tudo", "Cor do Vértice (R)", "Virado para Cima", "Manchas" }; // (MAT_WET_MASK_)
static const char* const WAVE_DIRS[] = { "X", "Y", "Z", "Diagonal / Esférico" };
static const char* const WAVE_PROFILES[] = { "Seno", "Serra", "Triângulo" };
static const char* const NOISE_DIMS[] = { "1D", "2D", "3D", "4D" };
static const char* const NOISE_TYPES[] = { "Multifractal", "fBM", "Multifractal híbrido", "Multifractal com cristas", "Terreno heterogêneo" };
static const char* const VORONOI_FEATURES[] = { "F1", "F2" };
static const char* const VORONOI_METRICS[] = { "Euclidiana", "Manhattan", "Chebychev" };
static const char* const MIX_TYPES[] = { "Float", "Vetor", "Cor" };
static const char* const BLEND_TYPES[BLEND_COUNT] = { "Mistura", "Adicionar", "Multiplicar", "Subtrair", "Tela", "Dividir",
                                                      "Diferença", "Escurecer", "Clarear", "Sobrepor", "Subexposição de cor",
                                                      "Superexposição de cor", "Matiz", "Saturação", "Valor", "Cor",
                                                      "Luz suave", "Luz linear", "Exclusão" };
static const char* const RAMP_INTERP[] = { "Linear", "Suave", "Constante", "B-Spline", "Cardinal" };
// (Q19) the Tlender's baked-only nodes' enums (its nodes_sim.inc, nodes.c)
static const char* const FREQUENCIES[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };
static const char* const MAGIC_DEPTHS[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };
static const char* const WEAVE_PATTERN_ITEMS[WEAVE_PATTERNS] = { "Tafetá", "Sarja", "Cetim", "Cesta" };
static const char* const FIBER_AXES[] = { "X", "Y", "Z" };
static const char* const VROT_TYPES[] = { "Eixo e ângulo", "Eixo X", "Eixo Y", "Eixo Z", "Euler XYZ" };
static const char* const MATH_OPS[MATH_COUNT] = {
    "Adicionar", "Subtrair", "Multiplicar", "Dividir", "Seno", "Cosseno", "Tangente", "Arco seno", "Arco cosseno",
    "Arco tangente", "Potência", "Logaritmo", "Mínimo", "Máximo", "Arredondar", "Menor que", "Maior que", "Módulo",
    "Absoluto", "Arctan2", "Piso", "Teto", "Fração", "Raiz quadrada", "Raiz quadrada inversa", "Sinal", "Exponencial",
    "Para radianos", "Para graus", "Seno hiperbólico", "Cosseno hiperbólico", "Tangente hiperbólica", "Truncar",
    "Encaixar", "Envolver", "Comparar", "Multiplicar e somar", "Pingue-pongue", "Mínimo suave", "Máximo suave",
    "Módulo arredondado para baixo"
};
static const char* const VMATH_OPS[VMATH_COUNT] = {
    "Adicionar", "Subtrair", "Multiplicar", "Dividir", "Produto vetorial", "Projetar", "Refletir", "Produto escalar",
    "Distância", "Comprimento", "Escalar", "Normalizar", "Encaixar", "Piso", "Teto", "Módulo", "Fração", "Absoluto",
    "Mínimo", "Máximo", "Envolver", "Seno", "Cosseno", "Tangente", "Refratar", "Virar para frente", "Multiplicar e somar"
};
static const char* const MAP_RANGE_TYPES[] = { "Linear", "Em degraus", "Smooth step", "Smoother step" };
static const char* const CLAMP_TYPES[] = { "Mín Máx", "Intervalo" };
static const char* const COLOR_MODES[] = { "RGB", "HSV", "HSL" };
static const char* const MAPPING_TYPES[] = { "Ponto", "Textura", "Vetor", "Normal" };
static const char* const IMAGE_INTERP[] = { "Linear", "Mais próximo" };
static const char* const IMAGE_EXTENSIONS[] = { "Repetir", "Estender", "Recortar", "Espelhar" };
static const char* const IMAGE_COLORSPACES[] = { "sRGB", "Non-Color" };
static const char* const IMAGE_ALPHA_MODES[] = { "Direto", "Pré-multiplicado", "Canais empacotados", "Nenhum" };
static const char* const NMAP_SPACES[] = { "Espaço tangente", "Espaço do objeto", "Espaço do mundo", "Objeto do Blender", "Mundo do Blender" };
static const char* const NMAP_CONVENTIONS[] = { "OpenGL", "DirectX" };

static void build(void) {
    MatNodeDef* d;
    g_count = 0;
    // --- Input ---
    d = def_begin(NODE_TEX_COORD, "ShaderNodeTexCoord", "Texture Coordinate", "Coordenadas de textura", MCAT_INPUT, 150);
    out(d, "Generated", "Gerado", MSOCK_VECTOR);
    out(d, "Normal", "Normal", MSOCK_VECTOR);
    out(d, "UV", "UV", MSOCK_VECTOR);
    out(d, "Object", "Objeto", MSOCK_VECTOR);
    out(d, "Camera", "Câmera", MSOCK_VECTOR);
    out(d, "Window", "Janela", MSOCK_VECTOR);
    out(d, "Reflection", "Reflexão", MSOCK_VECTOR);

    d = def_begin(NODE_FRESNEL, "ShaderNodeFresnel", "Fresnel", "Fresnel", MCAT_INPUT, 140);
    in_float(d, "IOR", "IOR", 1.5f, 0.0f, 1000.0f, 0);
    in_normal(d, "Normal", "Normal");
    out(d, "Fac", "Fator", MSOCK_FLOAT);

    d = def_begin(NODE_LAYER_WEIGHT, "ShaderNodeLayerWeight", "Layer Weight", "Peso de camada", MCAT_INPUT, 140);
    in_fac(d, "Blend", "Mistura", 0.5f);
    in_normal(d, "Normal", "Normal");
    out(d, "Fresnel", "Fresnel", MSOCK_FLOAT);
    out(d, "Facing", "Frontal", MSOCK_FLOAT);

    d = def_begin(NODE_RGB, "ShaderNodeRGB", "RGB", "RGB", MCAT_INPUT, 140);
    out(d, "Color", "Cor", MSOCK_COLOR);

    d = def_begin(NODE_VALUE, "ShaderNodeValue", "Value", "Valor", MCAT_INPUT, 140);
    out(d, "Value", "Valor", MSOCK_FLOAT);

    d = def_begin(NODE_UV_MAP, "ShaderNodeUVMap", "UV Map", "Mapa UV", MCAT_INPUT, 150);
    out(d, "UV", "UV", MSOCK_VECTOR);

    // the mesh's vertex colors (M5: glTF's COLOR_0; white where a mesh has none)
    d = def_begin(NODE_VERTEX_COLOR, "ShaderNodeVertexColor", "Color Attribute", "Atributo de cor", MCAT_INPUT, 150);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Alpha", "Alfa", MSOCK_FLOAT);

    // the Real Engine's: time for animated materials (the loop is baked into frames the renderer blends; Unreal's Time)
    d = def_begin(NODE_RE_TIME, "RealEngineTime", "Time", "Tempo", MCAT_INPUT, 150);
    in_float(d, "Period", "Período", 2.0f, 0.05f, 3600.0f, 0);
    out(d, "Time", "Tempo", MSOCK_FLOAT);
    out(d, "Phase", "Fase", MSOCK_FLOAT);
    out(d, "Sine", "Seno", MSOCK_FLOAT);
    out(d, "Cosine", "Cosseno", MSOCK_FLOAT);

    // (2026-11-17, Q4) the Real Engine's Interior Mapping (Interior Cubemap / Atlas): rooms behind the windows
    d = def_begin(NODE_RE_INTERIOR, "RealEngineInteriorMapping", "Interior Mapping", "Interiores (Parallax)", MCAT_SHADER, 220);
    static const char* const ATLAS_N[8] = { "1", "2", "3", "4", "5", "6", "7", "8" };
    prop(d, "Colunas do atlas", ATLAS_N, 8, 0);
    prop(d, "Linhas do atlas", ATLAS_N, 8, 0);
    in_shader(d, "Shader", "Shader");
    in_float(d, "Rooms U", "Salas em U", 4.0f, 1.0f, 256.0f, 0);
    in_float(d, "Rooms V", "Salas em V", 4.0f, 1.0f, 256.0f, 0);
    in_float(d, "Room Depth", "Profundidade da Sala", 1.0f, 0.1f, 4.0f, 0);
    in_float(d, "Frame", "Moldura", 0.08f, 0.0f, 0.45f, 0);
    in_fac(d, "Furniture", "Móveis", 0.7f);
    in_fac(d, "Curtains", "Cortinas", 0.5f);
    in_fac(d, "Night", "Noite", 0.0f);
    in_fac(d, "Lit Rooms", "Salas Acesas", 0.6f);
    in_color(d, "Light Color", "Cor da Luz", 1.0f, 0.78f, 0.5f);
    in_float(d, "Light Strength", "Força da Luz", 120.0f, 0.0f, 10000.0f, 0);
    in_float(d, "Seed", "Semente", 0.0f, 0.0f, 1000.0f, 0);
    out(d, "Shader", "Shader", MSOCK_SHADER);

    // (2026-11-16) the Real Engine's Wet Surface: a shader wet (rain, a splash, sweat), changed by a Blueprint at run time
    d = def_begin(NODE_RE_WET, "RealEngineWetSurface", "Wet Surface", "Superfície Molhada", MCAT_SHADER, 200);
    prop(d, "Máscara", WET_MASKS, MAT_WET_MASK_COUNT, MAT_WET_MASK_ALL);
    in_shader(d, "Shader", "Shader");
    in_fac(d, "Wetness", "Molhado", 0.7f);
    in_float(d, "Porosity", "Porosidade", -1.0f, -1.0f, 1.0f, 0);
    in_fac(d, "Water Film", "Película d'Água", 0.6f);
    in_fac(d, "Film Roughness", "Rugosidade da Película", 0.05f);
    in_fac(d, "Droplets", "Gotas", 0.3f);
    in_float(d, "Droplet Size", "Tamanho das Gotas", 3.0f, 0.3f, 20.0f, 0);
    in_fac(d, "Running", "Escorrendo", 0.2f);
    in_fac(d, "Sweat", "Suor", 0.0f);
    out(d, "Shader", "Shader", MSOCK_SHADER);

    // --- Shader ---
    d = def_begin(NODE_BSDF_PRINCIPLED, "ShaderNodeBsdfPrincipled", "Principled BSDF", "BSDF Principled", MCAT_SHADER, 240);
    prop(d, "Distribuição", DISTRIBUTIONS, 2, 0);
    prop(d, "Método da subsuperfície", SSS_METHODS, 3, 0);
    in_color(d, "Base Color", "Cor base", 0.8f, 0.8f, 0.8f);
    in_fac(d, "Metallic", "Metálico", 0.0f);
    in_fac(d, "Roughness", "Rugosidade", 0.5f);
    in_float(d, "IOR", "IOR", 1.5f, 1.0f, 1000.0f, 0);
    in_fac(d, "Alpha", "Alfa", 1.0f);
    in_normal(d, "Normal", "Normal");
    in_fac(d, "Diffuse Roughness", "Rugosidade difusa", 0.0f);
    in_fac(d, "Subsurface Weight", "Peso da subsuperfície", 0.0f);
    in_vector(d, "Subsurface Radius", "Raio da subsuperfície", 1.0f, 0.2f, 0.1f, 0);
    in_float(d, "Subsurface Scale", "Escala da subsuperfície", 0.05f, 0.0f, BIG, 0);
    in_float(d, "Subsurface IOR", "IOR da subsuperfície", 1.4f, 1.01f, 3.8f, 0);
    in_float(d, "Subsurface Anisotropy", "Anisotropia da subsuperfície", 0.0f, 0.0f, 1.0f, MSF_FACTOR);
    in_float(d, "Specular IOR Level", "Nível do IOR", 0.5f, 0.0f, 1.0f, MSF_FACTOR);
    in_color(d, "Specular Tint", "Tonalidade especular", 1.0f, 1.0f, 1.0f);
    in_fac(d, "Anisotropic", "Anisotrópico", 0.0f);
    in_fac(d, "Anisotropic Rotation", "Rotação anisotrópica", 0.0f);
    add_in(d, "Tangent", "Tangente", MSOCK_VECTOR, 0, 0, 0, -1, 1, MSF_HIDE_VALUE);
    in_fac(d, "Transmission Weight", "Peso da transmissão", 0.0f);
    in_fac(d, "Coat Weight", "Peso do revestimento", 0.0f);
    in_fac(d, "Coat Roughness", "Rugosidade do revestimento", 0.03f);
    in_float(d, "Coat IOR", "IOR do revestimento", 1.5f, 1.0f, 4.0f, 0);
    in_color(d, "Coat Tint", "Tonalidade do revestimento", 1.0f, 1.0f, 1.0f);
    in_normal(d, "Coat Normal", "Normal do revestimento");
    in_fac(d, "Sheen Weight", "Peso do aveludado", 0.0f);
    in_fac(d, "Sheen Roughness", "Rugosidade do aveludado", 0.5f);
    in_color(d, "Sheen Tint", "Tonalidade do aveludado", 1.0f, 1.0f, 1.0f);
    in_color(d, "Emission Color", "Cor da emissão", 1.0f, 1.0f, 1.0f);
    in_float(d, "Emission Strength", "Intensidade da emissão", 0.0f, 0.0f, 1000000.0f, 0);
    in_float(d, "Thin Film Thickness", "Espessura da película", 0.0f, 0.0f, 100000.0f, 0);
    in_float(d, "Thin Film IOR", "IOR da película", 1.33f, 1.0f, 1000.0f, 0);
    out(d, "BSDF", "BSDF", MSOCK_SHADER);

    d = def_begin(NODE_BSDF_DIFFUSE, "ShaderNodeBsdfDiffuse", "Diffuse BSDF", "BSDF Difuso", MCAT_SHADER, 150);
    in_color(d, "Color", "Cor", 0.8f, 0.8f, 0.8f);
    in_fac(d, "Roughness", "Rugosidade", 0.0f);
    in_normal(d, "Normal", "Normal");
    out(d, "BSDF", "BSDF", MSOCK_SHADER);

    d = def_begin(NODE_BSDF_GLOSSY, "ShaderNodeBsdfAnisotropic", "Glossy BSDF", "BSDF Brilhante", MCAT_SHADER, 160);
    prop(d, "Distribuição", DISTRIBUTIONS, 2, 0);
    in_color(d, "Color", "Cor", 0.8f, 0.8f, 0.8f);
    in_fac(d, "Roughness", "Rugosidade", 0.5f);
    in_fac(d, "Anisotropy", "Anisotropia", 0.0f);
    in_fac(d, "Rotation", "Rotação", 0.0f);
    in_normal(d, "Normal", "Normal");
    add_in(d, "Tangent", "Tangente", MSOCK_VECTOR, 0, 0, 0, -1, 1, MSF_HIDE_VALUE);
    out(d, "BSDF", "BSDF", MSOCK_SHADER);

    d = def_begin(NODE_BSDF_TRANSPARENT, "ShaderNodeBsdfTransparent", "Transparent BSDF", "BSDF Transparente", MCAT_SHADER, 150);
    in_color(d, "Color", "Cor", 1.0f, 1.0f, 1.0f);
    out(d, "BSDF", "BSDF", MSOCK_SHADER);

    d = def_begin(NODE_EMISSION, "ShaderNodeEmission", "Emission", "Emissão", MCAT_SHADER, 140);
    in_color(d, "Color", "Cor", 1.0f, 1.0f, 1.0f);
    in_float(d, "Strength", "Intensidade", 1.0f, 0.0f, 1000000.0f, 0);
    out(d, "Emission", "Emissão", MSOCK_SHADER);

    d = def_begin(NODE_MIX_SHADER, "ShaderNodeMixShader", "Mix Shader", "Misturar shader", MCAT_SHADER, 140);
    in_fac(d, "Fac", "Fator", 0.5f);
    in_shader(d, "Shader", "Shader");
    in_shader(d, "Shader_001", "Shader");
    out(d, "Shader", "Shader", MSOCK_SHADER);

    d = def_begin(NODE_ADD_SHADER, "ShaderNodeAddShader", "Add Shader", "Somar shader", MCAT_SHADER, 140);
    in_shader(d, "Shader", "Shader");
    in_shader(d, "Shader_001", "Shader");
    out(d, "Shader", "Shader", MSOCK_SHADER);

    // --- Texture ---
    d = def_begin(NODE_TEX_IMAGE, "ShaderNodeTexImage", "Image Texture", "Textura de imagem", MCAT_TEXTURE, 220);
    prop(d, "Interpolação", IMAGE_INTERP, 2, IMAGE_INTERP_LINEAR);
    prop(d, "Extensão", IMAGE_EXTENSIONS, 4, IMAGE_EXT_REPEAT);
    prop(d, "Espaço de cor", IMAGE_COLORSPACES, 2, IMAGE_COLORSPACE_SRGB);
    prop(d, "Modo do alfa", IMAGE_ALPHA_MODES, 4, IMAGE_ALPHA_STRAIGHT);
    add_in(d, "Vector", "Vetor", MSOCK_VECTOR, 0, 0, 0, -BIG, BIG, MSF_HIDE_VALUE | MSF_IMPLICIT_UV);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Alpha", "Alfa", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_NOISE, "ShaderNodeTexNoise", "Noise Texture", "Textura de ruído", MCAT_TEXTURE, 160);
    prop(d, "Dimensões", NOISE_DIMS, 4, 2);
    prop(d, "Tipo", NOISE_TYPES, 5, NOISE_FBM);
    prop_bool(d, "Normalizar", 1);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "W", "W", 0.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Detail", "Detalhe", 2.0f, 0.0f, 15.0f, 0);
    in_fac(d, "Roughness", "Rugosidade", 0.5f);
    in_float(d, "Lacunarity", "Lacunaridade", 2.0f, 0.0f, 1000.0f, 0);
    in_float(d, "Offset", "Deslocamento", 0.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Gain", "Ganho", 1.0f, 0.0f, 1000.0f, 0);
    in_float(d, "Distortion", "Distorção", 0.0f, -1000.0f, 1000.0f, 0);
    out(d, "Fac", "Fator", MSOCK_FLOAT);
    out(d, "Color", "Cor", MSOCK_COLOR);

    d = def_begin(NODE_TEX_VORONOI, "ShaderNodeTexVoronoi", "Voronoi Texture", "Textura Voronoi", MCAT_TEXTURE, 150);
    prop(d, "Característica", VORONOI_FEATURES, 2, VORONOI_F1);
    prop(d, "Métrica", VORONOI_METRICS, 3, VORONOI_EUCLIDEAN);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    in_fac(d, "Randomness", "Aleatoriedade", 1.0f);
    out(d, "Distance", "Distância", MSOCK_FLOAT);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Position", "Posição", MSOCK_VECTOR);

    d = def_begin(NODE_TEX_CHECKER, "ShaderNodeTexChecker", "Checker Texture", "Textura xadrez", MCAT_TEXTURE, 150);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_color(d, "Color1", "Cor 1", 0.8f, 0.8f, 0.8f);
    in_color(d, "Color2", "Cor 2", 0.2f, 0.2f, 0.2f);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Fac", "Fator", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_GRADIENT, "ShaderNodeTexGradient", "Gradient Texture", "Textura de gradiente", MCAT_TEXTURE, 150);
    prop(d, "Tipo", GRADIENTS, 7, GRADIENT_LINEAR);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Fac", "Fator", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_WAVE, "ShaderNodeTexWave", "Wave Texture", "Textura de ondas", MCAT_TEXTURE, 160);
    prop(d, "Tipo", WAVE_TYPES, 2, WAVE_BANDS);
    prop(d, "Direção", WAVE_DIRS, 4, 0);
    prop(d, "Perfil", WAVE_PROFILES, 3, WAVE_PROFILE_SIN);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Distortion", "Distorção", 0.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Detail", "Detalhe", 2.0f, 0.0f, 15.0f, 0);
    in_float(d, "Detail Scale", "Escala do detalhe", 1.0f, -1000.0f, 1000.0f, 0);
    in_fac(d, "Detail Roughness", "Rugosidade do detalhe", 0.5f);
    in_float(d, "Phase Offset", "Deslocamento de fase", 0.0f, -1000.0f, 1000.0f, 0);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Fac", "Fator", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_WHITE_NOISE, "ShaderNodeTexWhiteNoise", "White Noise Texture", "Textura de ruído branco", MCAT_TEXTURE, 150);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    out(d, "Value", "Valor", MSOCK_FLOAT);
    out(d, "Color", "Cor", MSOCK_COLOR);

    // (Q19, notas-dev/importar-tlender.md) the Tlender's textures the Engine bakes only (mat_vm.glsl under MAT_BAKE: into
    // the material's textures on the GPU, never per pixel): Blender's Brick and Magic, the Tlender's weave, cracks,
    // stones, grains, ocean and fibers (copied from its nodes_sim.inc: the same sockets, order and defaults)
    d = def_begin(NODE_TEX_BRICK, "ShaderNodeTexBrick", "Brick Texture", "Textura de tijolos", MCAT_TEXTURE, 170);
    prop(d, "Frequência do deslocamento", FREQUENCIES, 10, 1); // (the value: the item's index + 1)
    prop(d, "Frequência do achatamento", FREQUENCIES, 10, 1);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_color(d, "Color1", "Cor 1", 0.8f, 0.8f, 0.8f);
    in_color(d, "Color2", "Cor 2", 0.2f, 0.2f, 0.2f);
    in_color(d, "Mortar", "Argamassa", 0.0f, 0.0f, 0.0f);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Mortar Size", "Tamanho da argamassa", 0.02f, 0.0f, 0.125f, 0);
    in_fac(d, "Mortar Smooth", "Suavidade da argamassa", 0.1f);
    in_float(d, "Bias", "Viés", 0.0f, -1.0f, 1.0f, MSF_FACTOR);
    in_float(d, "Brick Width", "Largura do tijolo", 0.5f, 0.01f, 100.0f, 0);
    in_float(d, "Row Height", "Altura da fileira", 0.25f, 0.01f, 100.0f, 0);
    in_fac(d, "Offset", "Deslocamento da fileira", 0.5f);
    in_float(d, "Squash", "Achatamento", 1.0f, 0.0f, 99.0f, 0);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Fac", "Fator", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_MAGIC, "ShaderNodeTexMagic", "Magic Texture", "Textura mágica", MCAT_TEXTURE, 150);
    prop(d, "Profundidade", MAGIC_DEPTHS, 11, 2);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Distortion", "Distorção", 1.0f, -1000.0f, 1000.0f, 0);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Fac", "Fator", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_WEAVE, "TlShaderNodeTexWeave", "Weave Texture", "Textura de trama", MCAT_TEXTURE, 170);
    prop(d, "Padrão da trama", WEAVE_PATTERN_ITEMS, WEAVE_PATTERNS, WEAVE_PLAIN);
    add_in(d, "Vector", "Vetor", MSOCK_VECTOR, 0, 0, 0, -BIG, BIG, MSF_HIDE_VALUE | MSF_IMPLICIT_UV);
    in_float(d, "Scale", "Escala", 60.0f, 0.0f, 10000.0f, 0);
    in_fac(d, "Thread Width", "Largura do fio", 0.85f);
    in_color(d, "Warp Color", "Cor da urdidura", 0.62f, 0.62f, 0.62f);
    in_color(d, "Weft Color", "Cor da trama", 0.8f, 0.8f, 0.8f);
    in_fac(d, "Fibers", "Fibras", 0.5f);
    in_fac(d, "Randomness", "Aleatoriedade", 0.15f);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Height", "Altura", MSOCK_FLOAT);
    out(d, "Warp", "Urdidura", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_CRACKS, "TlShaderNodeTexCracks", "Cracks Texture", "Textura de rachaduras", MCAT_TEXTURE, 160);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Width", "Largura", 0.05f, 0.0f, 1.0f, MSF_FACTOR);
    in_float(d, "Distortion", "Distorção", 0.4f, 0.0f, 10.0f, 0);
    in_fac(d, "Detail", "Detalhe", 0.5f);
    in_fac(d, "Curl", "Curvatura das placas", 0.5f);
    in_fac(d, "Randomness", "Aleatoriedade", 1.0f);
    out(d, "Fac", "Fator", MSOCK_FLOAT);
    out(d, "Height", "Altura", MSOCK_FLOAT);
    out(d, "Random", "Aleatório", MSOCK_FLOAT);
    out(d, "Distance", "Distância", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_STONES, "TlShaderNodeTexStones", "Stones Texture", "Textura de pedras", MCAT_TEXTURE, 160);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Mortar Size", "Tamanho da argamassa", 0.08f, 0.0f, 1.0f, MSF_FACTOR);
    in_fac(d, "Roundness", "Arredondamento", 0.6f);
    in_float(d, "Distortion", "Distorção", 0.15f, 0.0f, 10.0f, 0);
    in_fac(d, "Randomness", "Aleatoriedade", 1.0f);
    out(d, "Fac", "Fator", MSOCK_FLOAT);
    out(d, "Height", "Altura", MSOCK_FLOAT);
    out(d, "Random", "Aleatório", MSOCK_FLOAT);
    out(d, "Color", "Cor", MSOCK_COLOR);

    d = def_begin(NODE_TEX_GRAINS, "TlShaderNodeTexGrains", "Grains Texture", "Textura de grãos", MCAT_TEXTURE, 160);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "Scale", "Escala", 150.0f, -100000.0f, 100000.0f, 0);
    in_fac(d, "Grain Size", "Tamanho do grão", 0.9f);
    in_fac(d, "Randomness", "Aleatoriedade", 1.0f);
    out(d, "Height", "Altura", MSOCK_FLOAT);
    out(d, "Random", "Aleatório", MSOCK_FLOAT);
    out(d, "Facet", "Faceta", MSOCK_VECTOR);
    out(d, "Gap", "Vão", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_OCEAN, "TlShaderNodeTexOcean", "Ocean Texture", "Textura de oceano", MCAT_TEXTURE, 170);
    prop_bool(d, "Repetível", 0);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "Scale", "Escala", 2.0f, 0.0f, 1000.0f, 0);
    in_float(d, "Direction", "Direção", 0.0f, -BIG, BIG, MSF_ANGLE);
    in_fac(d, "Spread", "Espalhamento", 0.5f);
    in_fac(d, "Choppiness", "Agitação", 0.5f);
    in_float(d, "Waves", "Ondas", 8.0f, 1.0f, 16.0f, 0);
    in_float(d, "Time", "Tempo", 0.0f, -BIG, BIG, 0);
    out(d, "Height", "Altura", MSOCK_FLOAT);
    out(d, "Foam", "Espuma", MSOCK_FLOAT);

    d = def_begin(NODE_TEX_FIBERS, "TlShaderNodeTexFibers", "Fibers Texture", "Textura de fibras", MCAT_TEXTURE, 160);
    prop(d, "Eixo", FIBER_AXES, 3, 2);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_float(d, "Scale", "Escala", 5.0f, -1000.0f, 1000.0f, 0);
    in_float(d, "Stretch", "Alongamento", 8.0f, 1.0f, 1000.0f, 0);
    in_float(d, "Detail", "Detalhe", 4.0f, 0.0f, 15.0f, 0);
    in_fac(d, "Roughness", "Rugosidade", 0.55f);
    in_float(d, "Distortion", "Distorção", 0.3f, 0.0f, 10.0f, 0);
    out(d, "Fac", "Fator", MSOCK_FLOAT);
    out(d, "Ridges", "Cristas", MSOCK_FLOAT);

    // --- Color ---
    d = def_begin(NODE_MIX, "ShaderNodeMix", "Mix", "Misturar", MCAT_COLOR, 150);
    prop(d, "Tipo de dado", MIX_TYPES, 3, MIX_DATA_COLOR);
    prop(d, "Modo", BLEND_TYPES, BLEND_COUNT, BLEND_MIX);
    prop_bool(d, "Limitar fator", 1);
    prop_bool(d, "Limitar resultado", 0);
    in_fac(d, "Factor", "Fator", 0.5f);
    in_float(d, "A_Float", "A", 0.0f, -BIG, BIG, 0);
    in_float(d, "B_Float", "B", 0.0f, -BIG, BIG, 0);
    in_vector(d, "A_Vector", "A", 0, 0, 0, 0);
    in_vector(d, "B_Vector", "B", 0, 0, 0, 0);
    in_color(d, "A_Color", "A", 0.5f, 0.5f, 0.5f);
    in_color(d, "B_Color", "B", 0.5f, 0.5f, 0.5f);
    out(d, "Result_Float", "Resultado", MSOCK_FLOAT);
    out(d, "Result_Vector", "Resultado", MSOCK_VECTOR);
    out(d, "Result_Color", "Resultado", MSOCK_COLOR);

    d = def_begin(NODE_INVERT, "ShaderNodeInvert", "Invert Color", "Inverter cor", MCAT_COLOR, 140);
    in_fac(d, "Fac", "Fator", 1.0f);
    in_color(d, "Color", "Cor", 0.0f, 0.0f, 0.0f);
    out(d, "Color", "Cor", MSOCK_COLOR);

    d = def_begin(NODE_HUE_SAT, "ShaderNodeHueSaturation", "Hue/Saturation/Value", "Matiz/Saturação/Valor", MCAT_COLOR, 150);
    in_fac(d, "Hue", "Matiz", 0.5f);
    in_float(d, "Saturation", "Saturação", 1.0f, 0.0f, 2.0f, MSF_FACTOR);
    in_float(d, "Value", "Valor", 1.0f, 0.0f, 2.0f, MSF_FACTOR);
    in_fac(d, "Fac", "Fator", 1.0f);
    in_color(d, "Color", "Cor", 0.8f, 0.8f, 0.8f);
    out(d, "Color", "Cor", MSOCK_COLOR);

    d = def_begin(NODE_BRIGHT_CONTRAST, "ShaderNodeBrightContrast", "Brightness/Contrast", "Brilho/Contraste", MCAT_COLOR, 140);
    in_color(d, "Color", "Cor", 1.0f, 1.0f, 1.0f);
    in_float(d, "Bright", "Brilho", 0.0f, -100.0f, 100.0f, 0);
    in_float(d, "Contrast", "Contraste", 0.0f, -100.0f, 100.0f, 0);
    out(d, "Color", "Cor", MSOCK_COLOR);

    d = def_begin(NODE_GAMMA, "ShaderNodeGamma", "Gamma", "Gama", MCAT_COLOR, 140);
    in_color(d, "Color", "Cor", 1.0f, 1.0f, 1.0f);
    in_float(d, "Gamma", "Gama", 1.0f, 0.001f, 10.0f, 0);
    out(d, "Color", "Cor", MSOCK_COLOR);

    // --- Vector ---
    d = def_begin(NODE_MAPPING, "ShaderNodeMapping", "Mapping", "Mapeamento", MCAT_VECTOR, 150);
    prop(d, "Tipo", MAPPING_TYPES, 4, MAPPING_POINT);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, MSF_HIDE_VALUE | MSF_IMPLICIT_GENERATED);
    in_vector(d, "Location", "Localização", 0, 0, 0, 0);
    in_vector(d, "Rotation", "Rotação", 0, 0, 0, MSF_ANGLE);
    in_vector(d, "Scale", "Escala", 1, 1, 1, 0);
    out(d, "Vector", "Vetor", MSOCK_VECTOR);

    d = def_begin(NODE_BUMP, "ShaderNodeBump", "Bump", "Relevo", MCAT_VECTOR, 150);
    prop_bool(d, "Inverter", 0);
    in_fac(d, "Strength", "Força", 1.0f);
    in_float(d, "Distance", "Distância", 1.0f, 0.0f, 1000.0f, 0);
    in_float(d, "Height", "Altura", 1.0f, -1000.0f, 1000.0f, MSF_HIDE_VALUE);
    in_normal(d, "Normal", "Normal");
    in_float(d, "Filter Width", "Largura do filtro", 0.1f, 0.0f, 10.0f, 0);
    out(d, "Normal", "Normal", MSOCK_VECTOR);

    d = def_begin(NODE_NORMAL_MAP, "ShaderNodeNormalMap", "Normal Map", "Mapa normal", MCAT_VECTOR, 150);
    prop(d, "Espaço", NMAP_SPACES, 5, NMAP_TANGENT);
    prop(d, "Convenção", NMAP_CONVENTIONS, 2, 0);
    in_float(d, "Strength", "Força", 1.0f, 0.0f, 10.0f, 0);
    in_color(d, "Color", "Cor", 0.5f, 0.5f, 1.0f);
    out(d, "Normal", "Normal", MSOCK_VECTOR);

    // --- Converter ---
    d = def_begin(NODE_COLOR_RAMP, "ShaderNodeValToRGB", "Color Ramp", "Rampa de cor", MCAT_CONVERTER, 220);
    prop(d, "Interpolação", RAMP_INTERP, 5, RAMP_LINEAR);
    in_fac(d, "Fac", "Fator", 0.5f);
    out(d, "Color", "Cor", MSOCK_COLOR);
    out(d, "Alpha", "Alfa", MSOCK_FLOAT);

    d = def_begin(NODE_MATH, "ShaderNodeMath", "Math", "Matemática", MCAT_CONVERTER, 150);
    prop(d, "Operação", MATH_OPS, MATH_COUNT, MATH_ADD);
    prop_bool(d, "Limitar", 0);
    in_float(d, "Value", "Valor", 0.5f, -BIG, BIG, 0);
    in_float(d, "Value_001", "Valor", 0.5f, -BIG, BIG, 0);
    in_float(d, "Value_002", "Valor", 0.5f, -BIG, BIG, 0);
    out(d, "Value", "Valor", MSOCK_FLOAT);

    d = def_begin(NODE_VECTOR_MATH, "ShaderNodeVectorMath", "Vector Math", "Matemática vetorial", MCAT_CONVERTER, 150);
    prop(d, "Operação", VMATH_OPS, VMATH_COUNT, VMATH_ADD);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, 0);
    in_vector(d, "Vector_001", "Vetor", 0, 0, 0, 0);
    in_vector(d, "Vector_002", "Vetor", 0, 0, 0, 0);
    in_float(d, "Scale", "Escala", 1.0f, -BIG, BIG, 0);
    out(d, "Vector", "Vetor", MSOCK_VECTOR);
    out(d, "Value", "Valor", MSOCK_FLOAT);

    d = def_begin(NODE_MAP_RANGE, "ShaderNodeMapRange", "Map Range", "Mapear intervalo", MCAT_CONVERTER, 150);
    prop(d, "Interpolação", MAP_RANGE_TYPES, 4, MAP_RANGE_LINEAR);
    prop_bool(d, "Limitar", 1);
    in_float(d, "Value", "Valor", 1.0f, -BIG, BIG, 0);
    in_float(d, "From Min", "De mín.", 0.0f, -BIG, BIG, 0);
    in_float(d, "From Max", "De máx.", 1.0f, -BIG, BIG, 0);
    in_float(d, "To Min", "Para mín.", 0.0f, -BIG, BIG, 0);
    in_float(d, "To Max", "Para máx.", 1.0f, -BIG, BIG, 0);
    in_float(d, "Steps", "Degraus", 4.0f, 0.0f, BIG, 0);
    out(d, "Result", "Resultado", MSOCK_FLOAT);

    d = def_begin(NODE_CLAMP, "ShaderNodeClamp", "Clamp", "Limitar", MCAT_CONVERTER, 140);
    prop(d, "Tipo", CLAMP_TYPES, 2, CLAMP_MINMAX);
    in_float(d, "Value", "Valor", 1.0f, -BIG, BIG, 0);
    in_float(d, "Min", "Mín.", 0.0f, -BIG, BIG, 0);
    in_float(d, "Max", "Máx.", 1.0f, -BIG, BIG, 0);
    out(d, "Result", "Resultado", MSOCK_FLOAT);

    d = def_begin(NODE_SEPARATE_XYZ, "ShaderNodeSeparateXYZ", "Separate XYZ", "Separar XYZ", MCAT_CONVERTER, 140);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, 0);
    out(d, "X", "X", MSOCK_FLOAT);
    out(d, "Y", "Y", MSOCK_FLOAT);
    out(d, "Z", "Z", MSOCK_FLOAT);

    d = def_begin(NODE_COMBINE_XYZ, "ShaderNodeCombineXYZ", "Combine XYZ", "Combinar XYZ", MCAT_CONVERTER, 140);
    in_float(d, "X", "X", 0.0f, -BIG, BIG, 0);
    in_float(d, "Y", "Y", 0.0f, -BIG, BIG, 0);
    in_float(d, "Z", "Z", 0.0f, -BIG, BIG, 0);
    out(d, "Vector", "Vetor", MSOCK_VECTOR);

    d = def_begin(NODE_SEPARATE_COLOR, "ShaderNodeSeparateColor", "Separate Color", "Separar cor", MCAT_CONVERTER, 140);
    prop(d, "Modo", COLOR_MODES, 3, COLOR_MODE_RGB);
    in_color(d, "Color", "Cor", 0.8f, 0.8f, 0.8f);
    out(d, "Red", "Vermelho", MSOCK_FLOAT);
    out(d, "Green", "Verde", MSOCK_FLOAT);
    out(d, "Blue", "Azul", MSOCK_FLOAT);

    d = def_begin(NODE_COMBINE_COLOR, "ShaderNodeCombineColor", "Combine Color", "Combinar cor", MCAT_CONVERTER, 140);
    prop(d, "Modo", COLOR_MODES, 3, COLOR_MODE_RGB);
    in_fac(d, "Red", "Vermelho", 0.0f);
    in_fac(d, "Green", "Verde", 0.0f);
    in_fac(d, "Blue", "Azul", 0.0f);
    out(d, "Color", "Cor", MSOCK_COLOR);

    d = def_begin(NODE_RGB_TO_BW, "ShaderNodeRGBToBW", "RGB to BW", "RGB para P&B", MCAT_CONVERTER, 140);
    in_color(d, "Color", "Cor", 0.5f, 0.5f, 0.5f);
    out(d, "Val", "Valor", MSOCK_FLOAT);

    // (Q19) baked only, as the textures above: Blender's Vector Rotate and Blackbody
    d = def_begin(NODE_VECTOR_ROTATE, "ShaderNodeVectorRotate", "Vector Rotate", "Rotação vetorial", MCAT_VECTOR, 150);
    prop(d, "Tipo", VROT_TYPES, 5, VROT_AXIS);
    prop_bool(d, "Inverter", 0);
    in_vector(d, "Vector", "Vetor", 0, 0, 0, 0);
    in_vector(d, "Center", "Centro", 0, 0, 0, 0);
    in_vector(d, "Axis", "Eixo", 0, 0, 1, 0);
    in_float(d, "Angle", "Ângulo", 0.0f, -BIG, BIG, MSF_ANGLE);
    in_vector(d, "Rotation", "Rotação", 0, 0, 0, MSF_ANGLE);
    out(d, "Vector", "Vetor", MSOCK_VECTOR);

    d = def_begin(NODE_BLACKBODY, "ShaderNodeBlackbody", "Blackbody", "Corpo negro", MCAT_CONVERTER, 140);
    in_float(d, "Temperature", "Temperatura", 1500.0f, 800.0f, 12000.0f, 0);
    out(d, "Color", "Cor", MSOCK_COLOR);

    // --- Output ---
    // Blender's Displacement: into the Material Output's Displacement, the height of a parallax (M7: an opaque material's
    // surface seen sunk into its height map; its Scale the depth)
    d = def_begin(NODE_DISPLACEMENT, "ShaderNodeDisplacement", "Displacement", "Deslocamento", MCAT_VECTOR, 150);
    in_float(d, "Height", "Altura", 0.0f, -BIG, BIG, 0);
    in_float(d, "Midlevel", "Nível médio", 0.5f, -BIG, BIG, 0);
    in_float(d, "Scale", "Escala", 1.0f, 0.0f, BIG, 0);
    in_normal(d, "Normal", "Normal");
    out(d, "Displacement", "Deslocamento", MSOCK_VECTOR);

    d = def_begin(NODE_OUTPUT_MATERIAL, "ShaderNodeOutputMaterial", "Material Output", "Saída do material", MCAT_OUTPUT, 140);
    in_shader(d, "Surface", "Superfície");
    in_shader(d, "Volume", "Volume");
    add_in(d, "Displacement", "Deslocamento", MSOCK_VECTOR, 0, 0, 0, -BIG, BIG, MSF_HIDE_VALUE);
    in_float(d, "Thickness", "Espessura", 0.0f, 0.0f, BIG, MSF_HIDE_VALUE);
    d = def_begin(MAT_NODE_REROUTE, "NodeReroute", "Reroute", "Redirecionar", MCAT_CONVERTER, 46); // a knot: what feeds it passes on
    in_color(d, "Input", "Entrada", 0, 0, 0);
    out(d, "Output", "Saída", MSOCK_COLOR);
    g_ready = true;
}

const MatNodeDef* mat_node_def(int type) {
    if (!g_ready) build();
    if (type < 0 || type > MAT_NODE_REROUTE || !g_defs[type].idname) return NULL;
    return &g_defs[type];
}

// What material_gpu.inc compiles: Principled BSDF reads Base Color, Metallic, Roughness, Alpha, Normal, Emission Color
// and Strength (0, 1, 2, 4, 5, 26, 27) per texel, and as the material's constants (principled_ext) IOR, Subsurface
// Weight / Radius / Scale, Specular IOR Level / Tint, Anisotropic and its Rotation, Transmission and Coat Weight /
// Roughness / IOR / Tint, Sheen Weight / Roughness / Tint, Thin Film Thickness / IOR (3, 7, 8, 9, 12, 13, 14, 15, 17,
// 18, 19, 20, 21, 23, 24, 25, 28, 29); the Glossy BSDF Color, Roughness and
// Normal (0, 1, 4). Update both together.
bool mat_input_has_effect(int type, int sock) {
    if (type == NODE_BSDF_PRINCIPLED)
        return sock == 0 || sock == 1 || sock == 2 || sock == 4 || sock == 5 || sock == 26 || sock == 27;
    if (type == NODE_BSDF_GLOSSY) return sock == 0 || sock == 1 || sock == 4;
    return true;
}
bool mat_input_const_only(int type, int sock) {
    if (type != NODE_BSDF_PRINCIPLED) return false;
    static const int k[] = { 3, 7, 8, 9, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24, 25, 28, 29 }; // (+ sheen, thin film: M3, M4)
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i] == sock) return true;
    return false;
}

// The Color Attribute's uses: its Color into the Principled BSDF's Base Color, directly or as a Mix (Multiply)'s input
// whose result goes there (A with any factor, B with the factor 1): the bake's white times the vertex color at run time
// is then the graph's own result. Anything else (its Alpha, other nodes) is baked white
static int skip_reroutes_out(const MatGraph* g, int li) { // (the link out of a reroute chain's end)
    for (int k = 0; k < 64 && li >= 0 && g->nodes[g->links[li].to].type == MAT_NODE_REROUTE; k++) {
        int r = g->links[li].to, nx = -1;
        for (int j = 0; j < g->link_count; j++)
            if (g->links[j].from == r) nx = j;
        li = nx;
    }
    return li;
}
static bool to_base_color(const MatGraph* g, int li) {
    li = skip_reroutes_out(g, li);
    return li >= 0 && g->nodes[g->links[li].to].type == NODE_BSDF_PRINCIPLED && g->links[li].to_sock == 0;
}
int mat_vcol_usage(const MatGraph* g) {
    if (!g) return MAT_VCOL_NONE;
    int use = MAT_VCOL_NONE;
    for (int i = 0; i < g->link_count; i++) {
        const MatLink* l = &g->links[i];
        if (g->nodes[l->from].type != NODE_VERTEX_COLOR) continue;
        bool ok = false;
        int li = skip_reroutes_out(g, i);
        if (li >= 0 && g->links[li].from_sock == 0) {
            const MatLink* t = &g->links[li];
            const MatNode* to = &g->nodes[t->to];
            if (to_base_color(g, li)) ok = true;
            else if (to->type == NODE_MIX && to->prop[0] == MIX_DATA_COLOR && to->prop[1] == BLEND_MUL &&
                     (t->to_sock == 5 || (t->to_sock == 6 && mat_graph_input_link(g, t->to, 0) < 0 && to->in[0][0] >= 0.999f))) {
                for (int j = 0; j < g->link_count && !ok; j++)
                    if (g->links[j].from == t->to && g->links[j].from_sock == 2) ok = to_base_color(g, j);
            }
        }
        use = ok && use != MAT_VCOL_OTHER ? MAT_VCOL_TINT : MAT_VCOL_OTHER;
    }
    return use;
}

int mat_node_unused_inputs(const MatGraph* g, int node, char* names, size_t cap) {
    if (cap) names[0] = 0;
    if (!g || node < 0 || node >= g->node_count) return 0;
    if (g->nodes[node].type == NODE_VERTEX_COLOR && mat_vcol_usage(g) == MAT_VCOL_OTHER) { // (only multiplying the Base Color counts)
        if (cap) snprintf(names, cap, "Color");
        return 1;
    }
    const MatNode* n = &g->nodes[node];
    const MatNodeDef* d = mat_node_def(n->type);
    if (!d) return 0;
    int count = 0;
    size_t at = 0;
    for (int i = 0; i < d->nin; i++) {
        if (mat_input_has_effect(n->type, i)) continue;
        const MatSockDef* s = &d->in[i];
        bool set = mat_graph_input_link(g, node, i) >= 0;
        if (!set && mat_input_const_only(n->type, i)) continue; // (its constant works: only a link is reported)
        int comps = s->type == MSOCK_FLOAT ? 1 : 3;
        for (int k = 0; k < comps && !set && !(s->flags & MSF_HIDE_VALUE); k++) set = fabsf(n->in[i][k] - s->def[k]) > 1e-4f;
        if (!set) continue;
        count++;
        if (cap && at + 1 < cap) {
            int w = snprintf(names + at, cap - at, "%s%s", at ? ", " : "", s->name);
            if (w > 0) at += (size_t)w < cap - at ? (size_t)w : cap - at - 1;
        }
    }
    return count;
}

const MatNodeDef* mat_node_def_by_idname(const char* idname) {
    if (!g_ready) build();
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_defs[g_order[i]].idname, idname)) return &g_defs[g_order[i]];
    return NULL;
}

int mat_node_defs(const MatNodeDef** outp, int cap) {
    if (!g_ready) build();
    int n = 0;
    for (int i = 0; i < g_count && n < cap; i++) outp[n++] = &g_defs[g_order[i]];
    return n;
}

const char* mat_category_name(int cat) {
    static const char* const names[MCAT_COUNT] = { "Input", "Shader", "Texture", "Color", "Vector", "Converter", "Output" };
    return cat >= 0 && cat < MCAT_COUNT ? names[cat] : "";
}

#define COL(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | 0xFF000000u)
uint32_t mat_category_color(int cat) { // Blender's node header colors
    static const uint32_t c[MCAT_COUNT] = { COL(131, 39, 39), COL(39, 120, 74), COL(121, 83, 35), COL(108, 105, 111),
                                            COL(56, 56, 112), COL(61, 104, 124), COL(99, 36, 36) };
    return cat >= 0 && cat < MCAT_COUNT ? c[cat] : COL(80, 80, 80);
}
uint32_t mat_socket_color(int type) {
    static const uint32_t c[4] = { COL(161, 161, 161), COL(99, 99, 199), COL(199, 199, 41), COL(99, 199, 99) };
    return type >= 0 && type < 4 ? c[type] : COL(160, 160, 160);
}
