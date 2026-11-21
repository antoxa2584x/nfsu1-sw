/* FLARE_FIX -- RECOMP_FLARE_GLARE=1 (default 0): a soft warm glare around
 * the bright lights only.
 *
 * Source: the title's light-trail buffer (flare_fix.h), which holds the
 * light flares and a blurred copy of the scene but not the scene itself,
 * so street lamps and car lights glare and the rest of the picture does
 * not. Only its hottest texels count: max(r, g, b) is smoothstep'ed from
 * the threshold to 1, so a light facing the camera (oncoming headlights,
 * saturated over a bigger area) glares more than one seen from the side.
 *
 * One full-screen pass at flip time, added (ONE / ONE, RGB only) onto the
 * finished frame before it is presented or dumped -- like the FXAA pass,
 * outside the title's own draws, so no per-draw CPU work and no state the
 * title's draws rely on is touched. Per pixel: four short streaks (+-x, +-y,
 * 8 taps each, quadratic fall-off) and a ring of 8 taps for the halo, tinted
 * warm white (1.0, 0.95, 0.8). Offsets are in the title's pixels, so the
 * look does not change with the resolution scale.
 *
 * =2: even frames only, an A/B in one run for slow hosts (dump every odd
 * number of frames).
 *
 * Tunables (read once):
 *   RECOMP_FLARE_GLARE_STRENGTH  streaks, 0.5     (0 = none)
 *   RECOMP_FLARE_GLARE_LENGTH    streak length, title pixels, 20
 *   RECOMP_FLARE_GLARE_HALO      halo, 0.3
 *   RECOMP_FLARE_GLARE_RADIUS    halo radius, title pixels, 5
 *   RECOMP_FLARE_GLARE_THRESHOLD trail brightness that starts to glare, 0.2
 * Included after present_fxaa in nv2a_vk.c (uses its statics). */
#ifndef FLARE_GLARE_H
#define FLARE_GLARE_H

static const char s_fg_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D trail;\n"
    "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"   /* a: px, strength, length; b: threshold, halo, radius */
    "layout(location = 0) out vec4 o;\n"
    "vec3 hot(vec2 uv) {\n"
    "    vec3 c = textureLod(trail, uv, 0.0).rgb;\n"
    "    return c * smoothstep(pc.b.x, 1.0, max(c.r, max(c.g, c.b)));\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / vec2(textureSize(trail, 0));\n"
    "    vec2 px = pc.a.xy;\n"
    "    const vec2 D[4] = vec2[](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));\n"
    "    vec3 s = vec3(0.0), h = vec3(0.0);\n"
    "    for (int d = 0; d < 4; d++)\n"
    "        for (int i = 1; i <= 8; i++) {\n"
    "            float t = float(i) / 8.0;\n"
    "            s += hot(uv + D[d] * (t * pc.a.w) * px) * ((1.0 - t) * (1.0 - t));\n"
    "        }\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "        float a = float(i) * 0.7853982;\n"
    "        h += hot(uv + vec2(cos(a), sin(a)) * pc.b.z * px);\n"
    "    }\n"
    /* 4 rays x sum((1 - i/8)^2) = 4 x 2.1875: a saturated area adds `strength`. */
    "    vec3 g = s * (pc.a.z / 8.75) + h * (pc.b.y / 8.0);\n"
    "    o = vec4(g * vec3(1.0, 0.95, 0.8), 0.0);\n"
    "}\n";

static float flare_glare_env(const char *name, float def)
{
    const char *e = getenv(name);
    float v = e && *e ? (float)atof(e) : def;
    return v >= 0.0f && v < 1000.0f ? v : def;
}

static VkPipeline flare_glare_pipe(VkPipelineLayout *lay)
{
    static VkDescriptorSetLayout dsl;
    static VkPipelineLayout pl;
    static VkPipeline pipe;
    static int failed;
    VkDescriptorSetLayoutBinding bd = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;   /* every title surface (surf_get) */
    VkShaderModule vs, fs;

    *lay = pl;
    if (pipe || failed)
        return pipe;
    failed = 1;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    lci.bindingCount = 1;
    lci.pBindings = &bd;
    if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &dsl) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(s_dev, &pli, NULL, &pl) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    vs = compile(GLSLANG_STAGE_VERTEX, s_pp_vs);
    fs = compile(GLSLANG_STAGE_FRAGMENT, s_fg_fs);
    if (vs && fs) {
        memset(st, 0, sizeof st);
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs;
        st[1].pName = "main";
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        vp.viewportCount = vp.scissorCount = 1;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        memset(&ba, 0, sizeof ba);
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        rci.colorAttachmentCount = 1;
        rci.pColorAttachmentFormats = &fmt;
        ci.pNext = &rci;
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = pl;
        if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS)
            pipe = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(s_dev, vs, NULL);
    if (fs) vkDestroyShaderModule(s_dev, fs, NULL);
    if (!pipe)
        LOGE("[flare] glare pipeline failed; glare off\n");
    failed = !pipe;
    *lay = pl;
    return pipe;
}

/* At flip, s = the surface about to be presented (or dumped). */
static void flare_glare(VkSurf *s)
{
    static float pc[8];
    static VkSampler smp;
    VkPipelineLayout lay;
    VkPipeline pipe;
    VkSurf *t;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkViewport vp;
    VkRect2D sc;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };

    /* The trail was updated in the frame this flip ends (vk_flip has
     * already counted the new one). */
    if (!s_ff_glare || !s || s_ff_trail_frame + 1 != s_frame || (s_ff_glare == 2 && (s_frame & 1)))
        return;
    t = surf_find(s_ff_trail_va);
    if (!t || t == s || t->w != s->w || t->h != s->h)
        return;
    pipe = flare_glare_pipe(&lay);
    if (!pipe)
        return;
    if (!smp) {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(s_dev, &sci, NULL, &smp) != VK_SUCCESS)
            return;
        pc[2] = flare_glare_env("RECOMP_FLARE_GLARE_STRENGTH", 0.5f);
        pc[3] = flare_glare_env("RECOMP_FLARE_GLARE_LENGTH", 20.0f);
        pc[4] = flare_glare_env("RECOMP_FLARE_GLARE_THRESHOLD", 0.2f);
        pc[5] = flare_glare_env("RECOMP_FLARE_GLARE_HALO", 0.3f);
        pc[6] = flare_glare_env("RECOMP_FLARE_GLARE_RADIUS", 5.0f);
        if (pc[4] > 0.99f)
            pc[4] = 0.99f;
        fprintf(stderr, "[flare] glare on: strength %.2f length %.0f halo %.2f radius %.0f threshold %.2f\n",
                pc[2], pc[3], pc[5], pc[6], pc[4]);
    }
    pc[0] = 1.0f / (float)t->w;
    pc[1] = 1.0f / (float)t->h;
    end_rendering();
    barrier_all();
    ca.imageView = s->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = s->pw;
    ri.renderArea.extent.height = s->ph;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vp.x = vp.y = 0.0f;
    vp.width = (float)s->pw;
    vp.height = (float)s->ph;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    sc.offset.x = sc.offset.y = 0;
    sc.extent.width = s->pw;
    sc.extent.height = s->ph;
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    ii.sampler = smp;
    ii.imageView = t->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &wr);
    vkCmdPushConstants(s_cb, lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    barrier_all();
    s_dyn_valid = 0;
}

#endif
