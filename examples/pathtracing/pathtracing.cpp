#include "VulkanRaytracingSample.h"
#define VK_GLTF_MATERIAL_IDS
#include "VulkanglTFModel.h"
#include <random>
#define ENABLE_VALIDATION true

constexpr uint32_t WIDTH = 1280;
constexpr uint32_t HEIGHT = 720;
// it is recommended not less than 8 and should not be greater than 'stack_size' in glsl/pathtracing/raygen.rgen. 'stack_size' can be set freely.
// the smaller the value, the faster the ray tracing speed
constexpr uint32_t RECURSIVE_DEPTH = 10;
constexpr bool ENABLE_DIRECT_LIGHTING = true;
// Stratified sampling
// divide one pixel into SAMPLE_DIMENSION*SAMPLE_DEMENTION subpixels
// then uniform randomly generate one sample in each subpixels
// the sample count per frame should be limited in 1000
constexpr uint32_t SAMPLE_DIMENSION = 2;
// Thus, the total sample counts per pixel per frames is
constexpr uint32_t SAMPLE_COUNT = SAMPLE_DIMENSION * SAMPLE_DIMENSION;
// the sample results will be accumulated
// output a ppm every n frames
// output dir: ./out/build/**/bin/*.ppm
constexpr uint32_t OUTPUT_INTERVAL = 2500;
// more camera parameters could be set in VulkanExample(): VulkanRaytracingSample(ENABLE_VALIDATION)
constexpr glm::vec3 POSITION = glm::vec3(-0.5f, 5.0f, 3.5f);
constexpr glm::vec3 ROTATION = glm::vec3(-15.0f, 120.0f, 0.0f);
// light information should not be greater than 'maxLights' in glsl/pathtracing/closesthit.rchit. 'maxLights' can be set freely.
constexpr uint32_t LIGHT_COUNT = 1;
struct Light {
    glm::vec4 position;
    glm::vec3 color;
    float intensity;
};

struct LightBlock {
    Light lights[LIGHT_COUNT] {
        Light { { 2., 8., 1., 1. }, glm::vec3(1), 50. },
    };
} lightBlock;

class VulkanExample : public VulkanRaytracingSample {
public:
    AccelerationStructure bottomLevelAS {};
    AccelerationStructure topLevelAS {};

    vks::Buffer vertexBuffer;
    vks::Buffer indexBuffer;
    uint32_t indexCount;
    vks::Buffer transformBuffer;

    struct GeometryNode {
        uint64_t vertexBufferDeviceAddress;
        uint64_t indexBufferDeviceAddress;
        int32_t textureIndexBaseColor;
        int32_t textureIndexNormal;
    };
    vks::Buffer geometryNodesBuffer;

    std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups {};
    struct ShaderBindingTables {
        ShaderBindingTable raygen;
        ShaderBindingTable miss;
        ShaderBindingTable hit;
    } shaderBindingTables;

    vks::Texture2D texture;

    struct UniformData {
        glm::mat4 viewInverse;
        glm::mat4 projInverse;
        uint32_t frame { 0 };
        uint32_t randomSeed { 0 };
        uint32_t recursiveDepth { RECURSIVE_DEPTH };
        uint32_t sampleDimension { SAMPLE_DIMENSION };
        uint32_t lightCount { LIGHT_COUNT };
        uint32_t enableDirectLighting { ENABLE_DIRECT_LIGHTING };
    } uniformData;
    vks::Buffer ubo;
    vks::Buffer light;

    VkPipeline pipeline;
    VkPipelineLayout pipelineLayout;
    VkDescriptorSet descriptorSet;
    VkDescriptorSetLayout descriptorSetLayout;

    vkglTF::Model model;

    VkPhysicalDeviceDescriptorIndexingFeaturesEXT
        physicalDeviceDescriptorIndexingFeatures {};

    std::random_device r;
    std::default_random_engine e;

    VulkanExample()
        : VulkanRaytracingSample(ENABLE_VALIDATION)
    {
        title = "Path tracing glTF model";
        e.seed(r());
        // settings.overlay = false;
        width = WIDTH;
        height = HEIGHT;
        camera.flipY = true;
        camera.rotationSpeed *= 0.25f;
        camera.movementSpeed *= 3.;
        camera.type = Camera::CameraType::firstperson;
        camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 512.0f);
        camera.setRotation(ROTATION);
        camera.setTranslation(POSITION);

        enableExtensions();

        // Buffer device address requires the 64-bit integer feature to be enabled
        enabledFeatures.shaderInt64 = VK_TRUE;

        enabledDeviceExtensions.push_back(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
        enabledDeviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }

    ~VulkanExample()
    {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        deleteStorageImage();
        deleteAccelerationStructure(bottomLevelAS);
        deleteAccelerationStructure(topLevelAS);
        vertexBuffer.destroy();
        indexBuffer.destroy();
        transformBuffer.destroy();
        shaderBindingTables.raygen.destroy();
        shaderBindingTables.miss.destroy();
        shaderBindingTables.hit.destroy();
        ubo.destroy();
        light.destroy();
        geometryNodesBuffer.destroy();
        // Clean up resources
        vkFreeMemory(device, copyImage.memory, nullptr);
        vkDestroyImage(device, copyImage.image, nullptr);
    }

    /*
                    Create the bottom level acceleration structure that contains the
       scene's actual geometry (vertices, triangles)
    */
    void createBottomLevelAccelerationStructure()
    {
        // Use transform matrices from the glTF nodes
        std::vector<VkTransformMatrixKHR> transformMatrices {};
        for (auto node : model.linearNodes) {
            if (node->mesh) {
                for (auto primitive : node->mesh->primitives) {
                    if (primitive->indexCount > 0) {
                        VkTransformMatrixKHR transformMatrix {};
                        auto m = glm::mat3x4(glm::transpose(node->getMatrix()));
                        memcpy(&transformMatrix, (void*)&m, sizeof(glm::mat3x4));
                        transformMatrices.push_back(transformMatrix);
                    }
                }
            }
        }

        // Transform buffer
        VK_CHECK_RESULT(vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &transformBuffer,
            static_cast<uint32_t>(transformMatrices.size()) * sizeof(VkTransformMatrixKHR),
            transformMatrices.data()));

        // Build
        // One geometry per glTF node, so we can index materials using
        // gl_GeometryIndexEXT
        std::vector<uint32_t> maxPrimitiveCounts {};
        std::vector<VkAccelerationStructureGeometryKHR> geometries {};
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> buildRangeInfos {};
        std::vector<VkAccelerationStructureBuildRangeInfoKHR*> pBuildRangeInfos {};
        std::vector<GeometryNode> geometryNodes {};
        for (auto node : model.linearNodes) {
            if (node->mesh) {
                for (auto primitive : node->mesh->primitives) {
                    if (primitive->indexCount > 0 && primitive->material.baseColorTexture && primitive->material.normalTexture) {
                        VkDeviceOrHostAddressConstKHR vertexBufferDeviceAddress {};
                        VkDeviceOrHostAddressConstKHR indexBufferDeviceAddress {};
                        VkDeviceOrHostAddressConstKHR transformBufferDeviceAddress {};

                        vertexBufferDeviceAddress.deviceAddress = getBufferDeviceAddress(
                            model.vertices.buffer); // +primitive->firstVertex *
                        // sizeof(vkglTF::Vertex);
                        indexBufferDeviceAddress.deviceAddress = getBufferDeviceAddress(model.indices.buffer) + primitive->firstIndex * sizeof(uint32_t);
                        transformBufferDeviceAddress.deviceAddress = getBufferDeviceAddress(transformBuffer.buffer) + static_cast<uint32_t>(geometryNodes.size()) * sizeof(VkTransformMatrixKHR);

                        VkAccelerationStructureGeometryKHR geometry {};
                        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
                        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                        geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                        geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                        geometry.geometry.triangles.vertexData = vertexBufferDeviceAddress;
                        geometry.geometry.triangles.maxVertex = model.vertices.count;
                        // geometry.geometry.triangles.maxVertex = primitive->vertexCount;
                        geometry.geometry.triangles.vertexStride = sizeof(vkglTF::Vertex);
                        geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
                        geometry.geometry.triangles.indexData = indexBufferDeviceAddress;
                        geometry.geometry.triangles.transformData = transformBufferDeviceAddress;
                        geometries.push_back(geometry);
                        maxPrimitiveCounts.push_back(primitive->indexCount / 3);

                        VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo {};
                        buildRangeInfo.firstVertex = 0;
                        buildRangeInfo.primitiveOffset = 0; // primitive->firstIndex * sizeof(uint32_t);
                        buildRangeInfo.primitiveCount = primitive->indexCount / 3;
                        buildRangeInfo.transformOffset = 0;
                        buildRangeInfos.push_back(buildRangeInfo);

                        GeometryNode geometryNode {};
                        geometryNode.vertexBufferDeviceAddress = vertexBufferDeviceAddress.deviceAddress;
                        geometryNode.indexBufferDeviceAddress = indexBufferDeviceAddress.deviceAddress;
                        geometryNode.textureIndexBaseColor = primitive->material.baseColorTexture->index;
                        geometryNode.textureIndexNormal = primitive->material.normalTexture->index;
                        // @todo: map material id to global texture array
                        geometryNodes.push_back(geometryNode);
                    }
                }
            }
        }
        for (auto& rangeInfo : buildRangeInfos) {
            pBuildRangeInfos.push_back(&rangeInfo);
        }

        // @todo: stage to device
        VK_CHECK_RESULT(vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &geometryNodesBuffer,
            static_cast<uint32_t>(geometryNodes.size()) * sizeof(GeometryNode),
            geometryNodes.data()));

        // Get size info
        VkAccelerationStructureBuildGeometryInfoKHR
            accelerationStructureBuildGeometryInfo {};
        accelerationStructureBuildGeometryInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        accelerationStructureBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        accelerationStructureBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationStructureBuildGeometryInfo.geometryCount = static_cast<uint32_t>(geometries.size());
        accelerationStructureBuildGeometryInfo.pGeometries = geometries.data();

        VkAccelerationStructureBuildSizesInfoKHR
            accelerationStructureBuildSizesInfo {};
        accelerationStructureBuildSizesInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(
            device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &accelerationStructureBuildGeometryInfo, maxPrimitiveCounts.data(),
            &accelerationStructureBuildSizesInfo);

        createAccelerationStructure(bottomLevelAS, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, accelerationStructureBuildSizesInfo);

        /* createAccelerationStructureBuffer(bottomLevelAS,
                 accelerationStructureBuildSizesInfo);

         VkAccelerationStructureCreateInfoKHR accelerationStructureCreateInfo {};
         accelerationStructureCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
         accelerationStructureCreateInfo.buffer = bottomLevelAS.buffer;
         accelerationStructureCreateInfo.size = accelerationStructureBuildSizesInfo.accelerationStructureSize;
         accelerationStructureCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
         vkCreateAccelerationStructureKHR(device, &accelerationStructureCreateInfo,
                 nullptr, &bottomLevelAS.handle);

         VkAccelerationStructureDeviceAddressInfoKHR accelerationDeviceAddressInfo {};
         accelerationDeviceAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
         accelerationDeviceAddressInfo.accelerationStructure = bottomLevelAS.handle;
         bottomLevelAS.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(
                 device, &accelerationDeviceAddressInfo);*/

        // Create a small scratch buffer used during build of the bottom level
        // acceleration structure
        ScratchBuffer scratchBuffer = createScratchBuffer(
            accelerationStructureBuildSizesInfo.buildScratchSize);

        accelerationStructureBuildGeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        accelerationStructureBuildGeometryInfo.dstAccelerationStructure = bottomLevelAS.handle;
        accelerationStructureBuildGeometryInfo.scratchData.deviceAddress = scratchBuffer.deviceAddress;

        const VkAccelerationStructureBuildRangeInfoKHR* buildOffsetInfo = buildRangeInfos.data();

        // Build the acceleration structure on the device via a one-time command
        // buffer submission Some implementations may support acceleration structure
        // building on the host
        // (VkPhysicalDeviceAccelerationStructureFeaturesKHR->accelerationStructureHostCommands),
        // but we prefer device builds
        VkCommandBuffer commandBuffer = vulkanDevice->createCommandBuffer(
            VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
        vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1,
            &accelerationStructureBuildGeometryInfo,
            pBuildRangeInfos.data());
        vulkanDevice->flushCommandBuffer(commandBuffer, queue);

        deleteScratchBuffer(scratchBuffer);
    }

    /*
                    The top level acceleration structure contains the scene's object
       instances
    */
    void createTopLevelAccelerationStructure()
    {
        // We flip the matrix [1][1] = -1.0f to accomodate for the glTF up vector
        VkTransformMatrixKHR transformMatrix = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, -1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f
        };

        VkAccelerationStructureInstanceKHR instance {};
        instance.transform = transformMatrix;
        instance.instanceCustomIndex = 0;
        instance.mask = 0xFF;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = bottomLevelAS.deviceAddress;

        // Buffer for instance data
        vks::Buffer instancesBuffer;
        VK_CHECK_RESULT(vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &instancesBuffer, sizeof(VkAccelerationStructureInstanceKHR),
            &instance));

        VkDeviceOrHostAddressConstKHR instanceDataDeviceAddress {};
        instanceDataDeviceAddress.deviceAddress = getBufferDeviceAddress(instancesBuffer.buffer);

        VkAccelerationStructureGeometryKHR accelerationStructureGeometry {};
        accelerationStructureGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        accelerationStructureGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        accelerationStructureGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        accelerationStructureGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        accelerationStructureGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
        accelerationStructureGeometry.geometry.instances.data = instanceDataDeviceAddress;

        // Get size info
        /*
        The pSrcAccelerationStructure, dstAccelerationStructure, and mode members of
        pBuildInfo are ignored. Any VkDeviceOrHostAddressKHR members of pBuildInfo
        are ignored by this command, except that the hostAddress member of
        VkAccelerationStructureGeometryTrianglesDataKHR::transformData will be
        examined to check if it is NULL.*
        */
        VkAccelerationStructureBuildGeometryInfoKHR
            accelerationStructureBuildGeometryInfo {};
        accelerationStructureBuildGeometryInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        accelerationStructureBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        accelerationStructureBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationStructureBuildGeometryInfo.geometryCount = 1;
        accelerationStructureBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;

        uint32_t primitive_count = 1;

        VkAccelerationStructureBuildSizesInfoKHR
            accelerationStructureBuildSizesInfo {};
        accelerationStructureBuildSizesInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(
            device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &accelerationStructureBuildGeometryInfo, &primitive_count,
            &accelerationStructureBuildSizesInfo);

        createAccelerationStructure(topLevelAS, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, accelerationStructureBuildSizesInfo);

        // createAccelerationStructureBuffer(topLevelAS,
        //     accelerationStructureBuildSizesInfo);

        // VkAccelerationStructureCreateInfoKHR accelerationStructureCreateInfo {};
        // accelerationStructureCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        // accelerationStructureCreateInfo.buffer = topLevelAS.buffer;
        // accelerationStructureCreateInfo.size = accelerationStructureBuildSizesInfo.accelerationStructureSize;
        // accelerationStructureCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        // vkCreateAccelerationStructureKHR(device, &accelerationStructureCreateInfo,
        //     nullptr, &topLevelAS.handle);

        // Create a small scratch buffer used during build of the top level
        // acceleration structure
        ScratchBuffer scratchBuffer = createScratchBuffer(
            accelerationStructureBuildSizesInfo.buildScratchSize);

        VkAccelerationStructureBuildGeometryInfoKHR accelerationBuildGeometryInfo {};
        accelerationBuildGeometryInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        accelerationBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        accelerationBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationBuildGeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        accelerationBuildGeometryInfo.dstAccelerationStructure = topLevelAS.handle;
        accelerationBuildGeometryInfo.geometryCount = 1;
        accelerationBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;
        accelerationBuildGeometryInfo.scratchData.deviceAddress = scratchBuffer.deviceAddress;

        VkAccelerationStructureBuildRangeInfoKHR
            accelerationStructureBuildRangeInfo {};
        accelerationStructureBuildRangeInfo.primitiveCount = 1;
        accelerationStructureBuildRangeInfo.primitiveOffset = 0;
        accelerationStructureBuildRangeInfo.firstVertex = 0;
        accelerationStructureBuildRangeInfo.transformOffset = 0;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR*>
            accelerationBuildStructureRangeInfos = {
                &accelerationStructureBuildRangeInfo
            };

        // Build the acceleration structure on the device via a one-time command
        // buffer submission Some implementations may support acceleration structure
        // building on the host
        // (VkPhysicalDeviceAccelerationStructureFeaturesKHR->accelerationStructureHostCommands),
        // but we prefer device builds
        VkCommandBuffer commandBuffer = vulkanDevice->createCommandBuffer(
            VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
        vkCmdBuildAccelerationStructuresKHR(
            commandBuffer, 1, &accelerationBuildGeometryInfo,
            accelerationBuildStructureRangeInfos.data());
        vulkanDevice->flushCommandBuffer(commandBuffer, queue);

        // VkAccelerationStructureDeviceAddressInfoKHR accelerationDeviceAddressInfo {};
        // accelerationDeviceAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        // accelerationDeviceAddressInfo.accelerationStructure = topLevelAS.handle;

        deleteScratchBuffer(scratchBuffer);
        instancesBuffer.destroy();
    }

    /*
                    Create the Shader Binding Tables that binds the programs and top-level
       acceleration structure

                    SBT Layout used in this sample:

                                    /-----------\
                                    | raygen    |
                                    |-----------|
                                    | miss + shadow     |
                                    |-----------|
                                    | hit + any |
                                    \-----------/

    */
    void createShaderBindingTables()
    {
        const uint32_t handleSize = rayTracingPipelineProperties.shaderGroupHandleSize;
        const uint32_t handleSizeAligned = vks::tools::alignedSize(
            rayTracingPipelineProperties.shaderGroupHandleSize,
            rayTracingPipelineProperties.shaderGroupHandleAlignment);
        const uint32_t groupCount = static_cast<uint32_t>(shaderGroups.size());
        const uint32_t sbtSize = groupCount * handleSizeAligned;

        std::vector<uint8_t> shaderHandleStorage(sbtSize);
        VK_CHECK_RESULT(vkGetRayTracingShaderGroupHandlesKHR(
            device, pipeline, 0, groupCount, sbtSize, shaderHandleStorage.data()));

        createShaderBindingTable(shaderBindingTables.raygen, 1);
        createShaderBindingTable(shaderBindingTables.miss, 2);
        createShaderBindingTable(shaderBindingTables.hit, 2);

        // Copy handles
        memcpy(shaderBindingTables.raygen.mapped,
            shaderHandleStorage.data(), handleSize);
        // We are using two miss shaders, so we need to get two handles for the miss
        // shader binding table
        memcpy(shaderBindingTables.miss.mapped,
            shaderHandleStorage.data() + handleSizeAligned, handleSize * 2);
        memcpy(shaderBindingTables.hit.mapped,
            shaderHandleStorage.data() + handleSizeAligned * 3, handleSize * 2);
    }

    /*
                    Create our ray tracing pipeline
    */
    void createRayTracingPipeline()
    {
        // @todo:
        uint32_t imageCount { 0 };
        imageCount = static_cast<uint32_t>(model.textures.size());

        std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
            // Binding 0: Top level acceleration structure
            vks::initializers::descriptorSetLayoutBinding(
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                0),
            // Binding 1: Ray tracing result image
            vks::initializers::descriptorSetLayoutBinding(
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                1),
            // Binding 2: Uniform buffer
            vks::initializers::descriptorSetLayoutBinding(
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                2),
            vks::initializers::descriptorSetLayoutBinding(
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                3),
            // Binding 4: Geometry node information
            vks::initializers::descriptorSetLayoutBinding(
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                4),
            // Binding 5: Light information

            // Binding 5: All images used by the glTF model
            vks::initializers::descriptorSetLayoutBinding(
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                5, imageCount),

        };
        // Binding 3: Texture image
        // vks::initializers::descriptorSetLayoutBinding(
        //    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        //    VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
        //    3),
        // Unbound set
        VkDescriptorSetLayoutBindingFlagsCreateInfoEXT setLayoutBindingFlags {};
        setLayoutBindingFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
        setLayoutBindingFlags.bindingCount = static_cast<uint32_t>(setLayoutBindings.size());
        std::vector<VkDescriptorBindingFlagsEXT> descriptorBindingFlags = {
            0, 0, 0, 0, 0, VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT
        };
        setLayoutBindingFlags.pBindingFlags = descriptorBindingFlags.data();

        VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
        descriptorSetLayoutCI.pNext = &setLayoutBindingFlags;
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI,
            nullptr, &descriptorSetLayout));

        VkPipelineLayoutCreateInfo pipelineLayoutCI = vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayout, 1);

        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr,
            &pipelineLayout));

        /*
                        Setup ray tracing shader groups
        */
        std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

        // Ray generation group
        {
            shaderStages.push_back(
                loadShader(getShadersPath() + "pathtracing/raygen.rgen.spv",
                    VK_SHADER_STAGE_RAYGEN_BIT_KHR));
            VkRayTracingShaderGroupCreateInfoKHR shaderGroup {};
            shaderGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
            shaderGroups.push_back(shaderGroup);
        }

        // Miss group
        {
            shaderStages.push_back(
                loadShader(getShadersPath() + "pathtracing/miss.rmiss.spv",
                    VK_SHADER_STAGE_MISS_BIT_KHR));
            VkRayTracingShaderGroupCreateInfoKHR shaderGroup {};
            shaderGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
            shaderGroups.push_back(shaderGroup);
            // Second shader for shadows
            shaderStages.push_back(
                loadShader(getShadersPath() + "pathtracing/shadow.rmiss.spv",
                    VK_SHADER_STAGE_MISS_BIT_KHR));
            shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroups.push_back(shaderGroup);
        }

        // Closest hit group for doing texture lookups
        {
            shaderStages.push_back(
                loadShader(getShadersPath() + "pathtracing/closesthit.rchit.spv",
                    VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR));
            VkRayTracingShaderGroupCreateInfoKHR shaderGroup {};
            shaderGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
            shaderGroup.generalShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.closestHitShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
            // This group also uses an anyhit shader for doing transparency (see
            // anyhit.rahit for details)
            shaderStages.push_back(
                loadShader(getShadersPath() + "pathtracing/anyhit.rahit.spv",
                    VK_SHADER_STAGE_ANY_HIT_BIT_KHR));
            shaderGroup.anyHitShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroups.push_back(shaderGroup);
        }

        // Closet hit group for check visibility from light
        {
            shaderStages.push_back(
                loadShader(getShadersPath() + "pathtracing/shadow.rchit.spv",
                    VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR));
            VkRayTracingShaderGroupCreateInfoKHR shaderGroup {};
            shaderGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
            shaderGroup.generalShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.closestHitShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
            shaderStages.push_back(
                loadShader(getShadersPath() + "pathtracing/anyhit.rahit.spv",
                    VK_SHADER_STAGE_ANY_HIT_BIT_KHR));
            shaderGroup.anyHitShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroups.push_back(shaderGroup);
        }

        /*
                        Create the ray tracing pipeline
        */
        VkRayTracingPipelineCreateInfoKHR rayTracingPipelineCI {};
        rayTracingPipelineCI.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        rayTracingPipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
        rayTracingPipelineCI.pStages = shaderStages.data();
        rayTracingPipelineCI.groupCount = static_cast<uint32_t>(shaderGroups.size());
        rayTracingPipelineCI.pGroups = shaderGroups.data();
        rayTracingPipelineCI.maxPipelineRayRecursionDepth = 2;
        rayTracingPipelineCI.layout = pipelineLayout;
        VK_CHECK_RESULT(vkCreateRayTracingPipelinesKHR(
            device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rayTracingPipelineCI,
            nullptr, &pipeline));
    }

    /*
                    Create the descriptor sets used for the ray tracing dispatch
    */
    void createDescriptorSets()
    {
        // @todo
        uint32_t imageCount { 0 };
        imageCount = static_cast<uint32_t>(model.textures.size());

        std::vector<VkDescriptorPoolSize> poolSizes = {
            { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageCount },
            
        };
        VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, 1);
        VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolCreateInfo,
            nullptr, &descriptorPool));

        VkDescriptorSetVariableDescriptorCountAllocateInfoEXT
            variableDescriptorCountAllocInfo {};
        uint32_t variableDescCounts[] = { imageCount };
        variableDescriptorCountAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT;
        variableDescriptorCountAllocInfo.descriptorSetCount = 1;
        variableDescriptorCountAllocInfo.pDescriptorCounts = variableDescCounts;

        VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool,
            &descriptorSetLayout, 1);
        descriptorSetAllocateInfo.pNext = &variableDescriptorCountAllocInfo;
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo,
            &descriptorSet));

        VkWriteDescriptorSetAccelerationStructureKHR
            descriptorAccelerationStructureInfo
            = vks::initializers::writeDescriptorSetAccelerationStructureKHR();
        descriptorAccelerationStructureInfo.accelerationStructureCount = 1;
        descriptorAccelerationStructureInfo.pAccelerationStructures = &topLevelAS.handle;

        VkWriteDescriptorSet accelerationStructureWrite {};
        accelerationStructureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        // The specialized acceleration structure descriptor has to be chained
        accelerationStructureWrite.pNext = &descriptorAccelerationStructureInfo;
        accelerationStructureWrite.dstSet = descriptorSet;
        accelerationStructureWrite.dstBinding = 0;
        accelerationStructureWrite.descriptorCount = 1;
        accelerationStructureWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        VkDescriptorImageInfo storageImageDescriptor {
            VK_NULL_HANDLE, storageImage.view, VK_IMAGE_LAYOUT_GENERAL
        };

        std::vector<VkWriteDescriptorSet> writeDescriptorSets = {
            // Binding 0: Top level acceleration structure
            accelerationStructureWrite,
            // Binding 1: Ray tracing result image
            vks::initializers::writeDescriptorSet(descriptorSet,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                1, &storageImageDescriptor),
            // Binding 2: Uniform data
            vks::initializers::writeDescriptorSet(descriptorSet,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                2, &ubo.descriptor),
            vks::initializers::writeDescriptorSet(descriptorSet,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                3, &light.descriptor),
            // Binding 3: Geometry node information normal
            vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                4, &geometryNodesBuffer.descriptor),

        };

        // Image descriptors for the image array
        std::vector<VkDescriptorImageInfo> textureDescriptors {};
        for (auto texture : model.textures) {
            VkDescriptorImageInfo descriptor {};
            descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            descriptor.sampler = texture.sampler;
            ;
            descriptor.imageView = texture.view;
            textureDescriptors.push_back(descriptor);
        }

        VkWriteDescriptorSet writeDescriptorImgArray {};
        writeDescriptorImgArray.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptorImgArray.dstBinding = 5;
        writeDescriptorImgArray.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeDescriptorImgArray.descriptorCount = imageCount;
        writeDescriptorImgArray.dstSet = descriptorSet;
        writeDescriptorImgArray.pImageInfo = textureDescriptors.data();
        writeDescriptorSets.push_back(writeDescriptorImgArray);

        vkUpdateDescriptorSets(device,
            static_cast<uint32_t>(writeDescriptorSets.size()),
            writeDescriptorSets.data(), 0, VK_NULL_HANDLE);
    }

    /*
                    Create the uniform buffer used to pass matrices to the ray tracing ray
       generation shader
    */
    void createUniformBuffer()
    {
        VK_CHECK_RESULT(
            vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &ubo, sizeof(uniformData), &uniformData));
        VK_CHECK_RESULT(ubo.map());

        updateUniformBuffers();
        uniformData.frame = 0;
    }

    void createLightBuffer()
    {
        VK_CHECK_RESULT(
            vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &light, sizeof(lightBlock), &lightBlock));
        
        VK_CHECK_RESULT(light.map());
        memcpy(light.mapped, &lightBlock, sizeof(lightBlock));
        light.unmap();
    }

    /*
                    If the window has been resized, we need to recreate the storage image
       and it's descriptor
    */
    void handleResize()
    {
        // Recreate image
        createStorageImage(swapChain.colorFormat, { width, height, 1 });
        // Update descriptor
        VkDescriptorImageInfo storageImageDescriptor {
            VK_NULL_HANDLE, storageImage.view, VK_IMAGE_LAYOUT_GENERAL
        };
        VkWriteDescriptorSet resultImageWrite = vks::initializers::writeDescriptorSet(descriptorSet,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            1, &storageImageDescriptor);
        vkUpdateDescriptorSets(device, 1, &resultImageWrite, 0, VK_NULL_HANDLE);
        resized = false;
    }

    /*
                    Command buffer generation
    */
    void buildCommandBuffers()
    {
        if (resized) {
            handleResize();
        }

        VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

        VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        for (int32_t i = 0; i < drawCmdBuffers.size(); ++i) {
            VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

            vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
            vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout, 0, 1, &descriptorSet, 0, 0);

            VkStridedDeviceAddressRegionKHR emptySbtEntry = {};
            vkCmdTraceRaysKHR(
                drawCmdBuffers[i],
                &shaderBindingTables.raygen.stridedDeviceAddressRegion,
                &shaderBindingTables.miss.stridedDeviceAddressRegion,
                &shaderBindingTables.hit.stridedDeviceAddressRegion,
                &emptySbtEntry,
                width,
                height,
                1);

            vks::tools::setImageLayout(
                drawCmdBuffers[i],
                swapChain.images[i],
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                subresourceRange);

            vks::tools::setImageLayout(
                drawCmdBuffers[i],
                storageImage.image,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                subresourceRange);

            vks::tools::setImageLayout(
                drawCmdBuffers[i], copyImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, subresourceRange);

            VkImageCopy copyRegion {};
            copyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copyRegion.srcOffset = { 0, 0, 0 };
            copyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copyRegion.dstOffset = { 0, 0, 0 };
            copyRegion.extent = { width, height, 1 };
            vkCmdCopyImage(drawCmdBuffers[i], storageImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapChain.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
            vkCmdCopyImage(drawCmdBuffers[i], storageImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copyImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

            vks::tools::setImageLayout(
                drawCmdBuffers[i],
                swapChain.images[i],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                subresourceRange);

            vks::tools::setImageLayout(
                drawCmdBuffers[i],
                storageImage.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL,
                subresourceRange);

            vks::tools::setImageLayout(drawCmdBuffers[i], copyImage.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL,
                subresourceRange);

            VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
        }
    }

    void updateUniformBuffers()
    {
        uniformData.projInverse = glm::inverse(camera.matrices.perspective);
        uniformData.viewInverse = glm::inverse(camera.matrices.view);
        uniformData.frame++;
        std::uniform_int_distribution<uint32_t> u(0, 1919810);
        uniformData.randomSeed = u(e);
        memcpy(ubo.mapped, &uniformData, sizeof(uniformData));

        //VK_CHECK_RESULT(light.map());
        //memcpy(light.mapped, &lightBlock, sizeof(lightBlock));
        //light.unmap();
    }

    void getEnabledFeatures()
    {
        // Enable features required for ray tracing using feature chaining via pNext
        enabledBufferDeviceAddresFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        enabledBufferDeviceAddresFeatures.bufferDeviceAddress = VK_TRUE;

        enabledRayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        enabledRayTracingPipelineFeatures.rayTracingPipeline = VK_TRUE;
        enabledRayTracingPipelineFeatures.pNext = &enabledBufferDeviceAddresFeatures;

        enabledAccelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        enabledAccelerationStructureFeatures.accelerationStructure = VK_TRUE;
        enabledAccelerationStructureFeatures.pNext = &enabledRayTracingPipelineFeatures;

        physicalDeviceDescriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
        physicalDeviceDescriptorIndexingFeatures
            .shaderSampledImageArrayNonUniformIndexing
            = VK_TRUE;
        physicalDeviceDescriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
        physicalDeviceDescriptorIndexingFeatures
            .descriptorBindingVariableDescriptorCount
            = VK_TRUE;
        physicalDeviceDescriptorIndexingFeatures.pNext = &enabledAccelerationStructureFeatures;

        deviceCreatepNextChain = &physicalDeviceDescriptorIndexingFeatures;

        enabledFeatures.samplerAnisotropy = VK_TRUE;
    }

    void loadAssets()
    {
        vkglTF::memoryPropertyFlags = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        // model.loadFromFile(getAssetPath() +
        //                        "models/FlightHelmet/glTF/FlightHelmet.gltf",
        //                    vulkanDevice, queue);
        const uint32_t gltfLoadingFlags = vkglTF::FileLoadingFlags::FlipY;
        model.loadFromFile(getAssetPath() + "sponza/sponza.gltf",
            vulkanDevice, queue, gltfLoadingFlags);
    }

    void prepare()
    {
        VulkanRaytracingSample::prepare();

        loadAssets();

        // Create the acceleration structures used to render the ray traced scene
        createBottomLevelAccelerationStructure();
        createTopLevelAccelerationStructure();

        createStorageImage(swapChain.colorFormat, { width, height, 1 });
        createCopyImage();
        createLightBuffer();
        createUniformBuffer();
        createRayTracingPipeline();
        createShaderBindingTables();
        createDescriptorSets();
        buildCommandBuffers();
        prepared = true;
    }

    void draw()
    {
        VulkanExampleBase::prepareFrame();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
        VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
        VulkanExampleBase::submitFrame();
    }

    virtual void render()
    {
        if (!prepared)
            return;
        updateUniformBuffers();
        draw();
        std::cerr << "sample count:" << (uniformData.frame) * SAMPLE_COUNT << std::endl;
        if (uniformData.frame && uniformData.frame% OUTPUT_INTERVAL == 0)
			saveScreenshot();
    }

    struct CopyImage {
        VkImage image;
        VkDeviceMemory memory;
    } copyImage;

    void createCopyImage()
    {
        // Create the linear tiled destination image to copy to and to read the memory from
        VkImageCreateInfo imageCreateCI(vks::initializers::imageCreateInfo());
        imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
        // Note that vkCmdBlitImage (if supported) will also do format conversions if the swapchain color format would differ
        imageCreateCI.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageCreateCI.extent.width = width;
        imageCreateCI.extent.height = height;
        imageCreateCI.extent.depth = 1;
        imageCreateCI.arrayLayers = 1;
        imageCreateCI.mipLevels = 1;
        imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateCI.tiling = VK_IMAGE_TILING_LINEAR;
        imageCreateCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // Create the image

        VK_CHECK_RESULT(vkCreateImage(device, &imageCreateCI, nullptr, &copyImage.image));
        // Create memory to back up the image
        VkMemoryRequirements memRequirements;
        VkMemoryAllocateInfo memAllocInfo(vks::initializers::memoryAllocateInfo());
        vkGetImageMemoryRequirements(device, copyImage.image, &memRequirements);
        memAllocInfo.allocationSize = memRequirements.size;
        // Memory must be host visible to copy from
        memAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &copyImage.memory));
        VK_CHECK_RESULT(vkBindImageMemory(device, copyImage.image, copyImage.memory, 0));
    }

    void saveScreenshot()
    {

        // Get layout of the image (including row pitch)
        VkImageSubresource subResource { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
        VkSubresourceLayout subResourceLayout;
        vkGetImageSubresourceLayout(device, copyImage.image, &subResource, &subResourceLayout);

        // Map image memory so we can start copying from it
        const char* data;
        vkMapMemory(device, copyImage.memory, 0, VK_WHOLE_SIZE, 0, (void**)&data);
        data += subResourceLayout.offset;

        std::string filename = std::to_string((uniformData.frame) * SAMPLE_COUNT) + std::string(".ppm");

        std::ofstream file(filename, std::ios::out | std::ios::binary);

        // ppm header
        file << "P6\n"
             << width << "\n"
             << height << "\n"
             << 255 << "\n";

        // If source is BGR (destination is always RGB) and we can't use blit (which does automatic conversion), we'll have to manually swizzle color components
        bool colorSwizzle = false;
        // Check if source is BGR
        // Note: Not complete, only contains most common and basic BGR surface formats for demonstration purposes

        std::vector<VkFormat> formatsBGR = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM };
        colorSwizzle = (std::find(formatsBGR.begin(), formatsBGR.end(), swapChain.colorFormat) != formatsBGR.end());

        // ppm binary pixel data
        for (uint32_t y = 0; y < height; y++) {
            unsigned int* row = (unsigned int*)data;
            for (uint32_t x = 0; x < width; x++) {
                if (colorSwizzle) {
                    file.write((char*)row + 2, 1);
                    file.write((char*)row + 1, 1);
                    file.write((char*)row, 1);
                } else {
                    file.write((char*)row, 3);
                }
                row++;
            }
            data += subResourceLayout.rowPitch;
        }
        file.close();

        std::cerr << "Screenshot saved to disk" << std::endl;
        vkUnmapMemory(device, copyImage.memory);
    }

    virtual void viewChanged() { uniformData.frame = -1; }
};

VULKAN_EXAMPLE_MAIN()
