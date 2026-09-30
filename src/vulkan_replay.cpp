#include "shader_lab/vulkan_replay.hpp"
#include "shader_lab/host_profile.hpp"
#define VK_NO_PROTOTYPES
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace sl {
namespace {
void require(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
void checked(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw VulkanReplayFailure(std::string(operation) + " failed: " + std::to_string(result));
}
#define SL_VK_FUNCTIONS(X)                                                                         \
    X(DestroyInstance)                                                                             \
    X(EnumeratePhysicalDevices)                                                                    \
    X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceFeatures2)                                  \
        X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties)             \
            X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer)    \
                X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
                    X(MapMemory) X(UnmapMemory) X(CreateShaderModule) X(DestroyShaderModule) X(    \
                        CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)                   \
                        X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) \
                            X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool)    \
                                X(AllocateDescriptorSets) X(UpdateDescriptorSets)                  \
                                    X(CreateCommandPool) X(DestroyCommandPool)                     \
                                        X(AllocateCommandBuffers) X(BeginCommandBuffer)            \
                                            X(EndCommandBuffer) X(CmdBindPipeline)                 \
                                                X(CmdBindDescriptorSets) X(CmdPushConstants)       \
                                                    X(CmdDispatch) X(CmdPipelineBarrier)           \
                                                        X(CreateFence) X(DestroyFence)             \
                                                            X(QueueSubmit) X(WaitForFences)
struct Vulkan {
#ifdef _WIN32
    HMODULE library = nullptr;
#else
    void *library = nullptr;
#endif
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        size_t size = 0;
    };
    std::vector<Buffer> buffers;
#define DECLARE(name) PFN_vk##name name = nullptr;
    SL_VK_FUNCTIONS(DECLARE)
#undef DECLARE
    Vulkan() = default;
    Vulkan(const Vulkan &) = delete;
    Vulkan &operator=(const Vulkan &) = delete;
    ~Vulkan() {
        if (device) {
            if (fence)
                DestroyFence(device, fence, nullptr);
            if (command_pool)
                DestroyCommandPool(device, command_pool, nullptr);
            if (pipeline)
                DestroyPipeline(device, pipeline, nullptr);
            if (descriptor_pool)
                DestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (pipeline_layout)
                DestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (set_layout)
                DestroyDescriptorSetLayout(device, set_layout, nullptr);
            if (module)
                DestroyShaderModule(device, module, nullptr);
            for (auto &b : buffers) {
                if (b.mapped)
                    UnmapMemory(device, b.memory);
                if (b.buffer)
                    DestroyBuffer(device, b.buffer, nullptr);
                if (b.memory)
                    FreeMemory(device, b.memory, nullptr);
            }
            DestroyDevice(device, nullptr);
        }
        if (instance && DestroyInstance)
            DestroyInstance(instance, nullptr);
#ifdef _WIN32
        if (library)
            FreeLibrary(library);
#else
        if (library)
            dlclose(library);
#endif
    }
    void load() {
#ifdef _WIN32
        library = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto get = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                                 GetProcAddress(library, "vkGetInstanceProcAddr"))
                           : nullptr;
#else
        library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        auto get = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                                 dlsym(library, "vkGetInstanceProcAddr"))
                           : nullptr;
#endif
        require(get != nullptr, "Vulkan loader unavailable");
        const auto create =
            reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
        require(create != nullptr, "Vulkan instance entry point unavailable");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Shader Lab isolated replay";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        checked(create(&info, nullptr, &instance), "vkCreateInstance");
#define LOAD(name)                                                                                 \
    name = reinterpret_cast<PFN_vk##name>(get(instance, "vk" #name));                              \
    require(name != nullptr, "missing vk" #name);
        SL_VK_FUNCTIONS(LOAD)
#undef LOAD
    }
    size_t buffer(VkPhysicalDevice physical, Bytes bytes) {
        buffers.emplace_back();
        auto &b = buffers.back();
        b.size = bytes.size();
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes.size();
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checked(CreateBuffer(device, &info, nullptr, &b.buffer), "vkCreateBuffer");
        VkMemoryRequirements needed{};
        GetBufferMemoryRequirements(device, b.buffer, &needed);
        VkPhysicalDeviceMemoryProperties memory{};
        GetPhysicalDeviceMemoryProperties(physical, &memory);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((needed.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = i;
                break;
            }
        require(type != UINT32_MAX, "replay requires coherent host-visible storage memory");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = needed.size;
        allocate.memoryTypeIndex = type;
        checked(AllocateMemory(device, &allocate, nullptr, &b.memory), "vkAllocateMemory");
        checked(BindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        checked(MapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
        std::memcpy(b.mapped, bytes.data(), bytes.size());
        return buffers.size() - 1;
    }
};
#undef SL_VK_FUNCTIONS
std::vector<uint8_t> encode(const std::vector<uint32_t> &words) {
    std::vector<uint8_t> bytes;
    for (auto word : words)
        for (unsigned i = 0; i < 4; ++i)
            bytes.push_back(uint8_t(word >> (8 * i)));
    return bytes;
}
} // namespace
VulkanReplayResult vulkan_replay(const ExecutionInputs &inputs, Bytes spirv, const json &layout) {
    require(layout.at("schema") == 1 && layout.at("status") == "exported" &&
                layout.at("spirv_valid") == true && layout.at("identity").at("stage") == "CS",
            "replay needs a validated compute layout");
    const auto text = inputs.profile.dump();
    require(layout.at("identity").at("header_sha256") == sha256(inputs.header) &&
                layout.at("identity").at("code_sha256") == sha256(inputs.code) &&
                layout.at("identity").at("spirv_sha256") == sha256(spirv) &&
                layout.at("identity").at("profile_json_sha256") ==
                    sha256(Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size())),
            "compiler artifact identity mismatch");
    const auto &profile = inputs.profile;
    const auto &execution = inputs.execution;
    require(profile.at("mode") == "context_snapshot" && profile.at("stage") == "CS" &&
                profile.at("wave_size") == execution.at("wave_size") &&
                layout.at("wave_size") == execution.at("wave_size"),
            "replay requires explicit matching compute profile and guest wave");
    const auto wave = execution.at("wave_size").get<uint32_t>();
    require(execution.at("exec_mask") == (wave == 32 ? "00000000ffffffff" : "ffffffffffffffff"),
            "partial initial EXEC is not implemented by Vulkan replay");
    const auto &compute = profile.at("compute");
    require(compute.at("threads") == execution.at("workgroup_size") &&
                compute.value("lds_size_dwords", 0u) == 0 &&
                compute.value("scratch_size_dwords", 0u) == 0 &&
                integer(inputs.header, 0x54, 2) == 0 && layout.at("scratch_dwords") == 0,
            "compute shape mismatch or unsupported LDS/scratch state");
    require(!layout.at("uses_dma").get<bool>() && layout.at("images").empty() &&
                layout.at("samplers").empty(),
            "BDA, images and samplers are not implemented by this replay backend");
    require(!spirv.empty() && spirv.size() % 4 == 0 && spirv.size() <= 64 * 1024 * 1024,
            "invalid module extent");
    const auto subgroup = compiler_host_subgroup(profile);
    const auto inventory = assess_spirv_host(spirv, nullptr);
    bool subgroup_dependent = false;
    for (const auto &cap : inventory.at("capabilities"))
        subgroup_dependent |= cap.get<uint32_t>() >= 61 && cap.get<uint32_t>() <= 68;
    std::map<uint32_t, std::string> resource_names;
    for (const auto &binding : layout.at("descriptors")) {
        const auto kind = binding.at("kind").get<uint32_t>();
        require(binding.at("set") == 0 && binding.at("binding") == kind &&
                    binding.at("descriptor_type") == "storage_buffer" &&
                    (kind == 0 || kind == 48 || kind == 49),
                "unsupported compute descriptor kind");
        if (kind != 0)
            continue;
        for (const auto &index : binding.at("resource_indices")) {
            const auto resource = index.get<uint32_t>();
            const auto &buffer = layout.at("buffers").at(resource);
            require(buffer.at("image_alias").is_null(),
                    "image-buffer aliases require image replay");
            const auto address =
                std::stoull(buffer.at("guest_address").get<std::string>(), nullptr, 16);
            const auto size = buffer.at("descriptor_size_bytes").get<uint64_t>();
            bool found = false;
            for (auto it = inputs.resources.begin(); it != inputs.resources.end(); ++it) {
                if (std::stoull(it.value().at("guest_address").get<std::string>(), nullptr, 16) !=
                    address)
                    continue;
                require(it.value().at("kind") == "buffer" && size &&
                            size <= inputs.resource_bytes.at(it.key()).size(),
                        "live descriptor range is not backed by a complete fixture buffer");
                require(
                    (!buffer.at("written").get<bool>() || it.value().at("access") != "read_only") &&
                        (!buffer.at("read").get<bool>() || it.value().at("access") != "write_only"),
                    "compiler access disagrees with fixture permissions");
                resource_names[resource] = it.key();
                found = true;
                break;
            }
            require(found, "live buffer descriptor lacks an exact-base fixture resource");
        }
    }
    for (const auto &resource : inputs.resources)
        require(resource.at("kind") == "buffer", "image fixtures require image replay");
    Vulkan vk;
    vk.load();
    uint32_t device_count = 0;
    checked(vk.EnumeratePhysicalDevices(vk.instance, &device_count, nullptr),
            "vkEnumeratePhysicalDevices");
    require(device_count && device_count <= 64, "no bounded Vulkan physical-device inventory");
    std::vector<VkPhysicalDevice> devices(device_count);
    checked(vk.EnumeratePhysicalDevices(vk.instance, &device_count, devices.data()),
            "vkEnumeratePhysicalDevices");
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceVulkan12Properties props12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties props13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties groups{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &groups;
    groups.pNext = &props12;
    props12.pNext = &props13;
    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features13;
    bool controlled = false;
    uint32_t family = UINT32_MAX;
    for (auto candidate : devices) {
        vk.GetPhysicalDeviceProperties2(candidate, &properties);
        vk.GetPhysicalDeviceFeatures2(candidate, &features);
        if (properties.properties.apiVersion < VK_API_VERSION_1_3)
            continue;
        controlled = features13.subgroupSizeControl &&
                     (props13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                     subgroup >= props13.minSubgroupSize && subgroup <= props13.maxSubgroupSize;
        if (subgroup_dependent && !controlled && groups.subgroupSize != subgroup)
            continue;
        if (!subgroup_dependent)
            controlled = false;
        uint32_t count = 0;
        vk.GetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        if (!count || count > 256)
            continue;
        std::vector<VkQueueFamilyProperties> queues(count);
        vk.GetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
        for (uint32_t i = 0; i < count; ++i)
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                family = i;
                break;
            }
        if (family != UINT32_MAX) {
            physical = candidate;
            break;
        }
    }
    require(physical != VK_NULL_HANDLE,
            "no Vulkan 1.3 compute device supports the compiler's subgroup size");
    const auto &limits = properties.properties.limits;
    const auto dispatch = execution.at("dispatch_size").get<std::array<uint32_t, 3>>();
    for (size_t i = 0; i < 3; ++i)
        require(dispatch[i] <= limits.maxComputeWorkGroupCount[i],
                "dispatch exceeds queried device limit");
    json host = {{"schema", 1},
                 {"api_version", "1.3"},
                 {"subgroup_size", controlled ? subgroup : groups.subgroupSize},
                 {"enabled_features",
                  {{"shaderInt64", bool(features.features.shaderInt64)},
                   {"shaderFloat64", bool(features.features.shaderFloat64)}}},
                 {"enabled_extensions", json::array()},
                 {"properties", json::object()},
                 {"subgroup_operations", json::array()},
                 {"subgroup_stages", json::array()},
                 {"limits",
                  {{"maxComputeWorkGroupSize",
                    {limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupSize[1],
                     limits.maxComputeWorkGroupSize[2]}},
                   {"maxComputeWorkGroupInvocations", limits.maxComputeWorkGroupInvocations}}}};
#define FLOAT_PROPERTY(name) host["properties"][#name] = bool(props12.name);
    FLOAT_PROPERTY(shaderDenormPreserveFloat16)
    FLOAT_PROPERTY(shaderDenormPreserveFloat32)
    FLOAT_PROPERTY(shaderDenormPreserveFloat64) FLOAT_PROPERTY(shaderDenormFlushToZeroFloat16)
        FLOAT_PROPERTY(shaderDenormFlushToZeroFloat32)
            FLOAT_PROPERTY(shaderDenormFlushToZeroFloat64)
                FLOAT_PROPERTY(shaderSignedZeroInfNanPreserveFloat16)
                    FLOAT_PROPERTY(shaderSignedZeroInfNanPreserveFloat32)
                        FLOAT_PROPERTY(shaderSignedZeroInfNanPreserveFloat64)
                            FLOAT_PROPERTY(shaderRoundingModeRTEFloat16)
                                FLOAT_PROPERTY(shaderRoundingModeRTEFloat32)
                                    FLOAT_PROPERTY(shaderRoundingModeRTEFloat64)
                                        FLOAT_PROPERTY(shaderRoundingModeRTZFloat16)
                                            FLOAT_PROPERTY(shaderRoundingModeRTZFloat32)
                                                FLOAT_PROPERTY(shaderRoundingModeRTZFloat64)
#undef FLOAT_PROPERTY
                                                    const char *operations[] = {
                                                        "basic",     "vote",    "arithmetic",
                                                        "ballot",    "shuffle", "shuffle_relative",
                                                        "clustered", "quad"};
    for (unsigned i = 0; i < 8; ++i)
        if (groups.supportedOperations & (1u << i))
            host["subgroup_operations"].push_back(operations[i]);
    if (groups.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)
        host["subgroup_stages"].push_back("CS");
    auto assessment = assess_spirv_host(spirv, host);
    assessment["profile_provenance"] = "queried_device_and_selected_enabled_features";
    require(assessment.at("status") == "satisfied",
            "queried Vulkan device does not satisfy all modeled module requirements");
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enable{};
    enable.robustBufferAccess = features.features.robustBufferAccess;
    require(enable.robustBufferAccess, "robust buffer access is required for replay");
    enable.shaderInt64 = features.features.shaderInt64;
    enable.shaderFloat64 = features.features.shaderFloat64;
    enable.shaderStorageBufferArrayDynamicIndexing =
        features.features.shaderStorageBufferArrayDynamicIndexing;
    VkPhysicalDeviceVulkan13Features enable13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enable13.subgroupSizeControl = controlled;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &enable13;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &enable;
    checked(vk.CreateDevice(physical, &device_info, nullptr, &vk.device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vk.GetDeviceQueue(vk.device, family, 0, &queue);
    std::map<std::string, size_t> allocations;
    for (const auto &[name, bytes] : inputs.resource_bytes) {
        require(bytes.size() <= limits.maxStorageBufferRange,
                "fixture buffer exceeds storage range limit");
        allocations[name] = vk.buffer(physical, bytes);
    }
    std::vector<uint32_t> shader_data;
    for (const auto &word : layout.at("shader_data").at("words"))
        shader_data.push_back(word.is_null() ? 0 : word.get<uint32_t>());
    const auto shader_data_bytes = encode(shader_data);
    // Each guest base owns a separately allocated VkBuffer bound at offset zero; packed offsets are
    // zero.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<std::vector<VkDescriptorBufferInfo>> buffer_infos;
    uint32_t total_descriptors = 0;
    for (const auto &binding : layout.at("descriptors")) {
        const auto kind = binding.at("kind").get<uint32_t>();
        const auto count = binding.at("descriptor_count").get<uint32_t>();
        bindings.push_back(
            {kind, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        buffer_infos.emplace_back();
        auto &infos = buffer_infos.back();
        if (kind == 0) {
            for (const auto &index : binding.at("resource_indices")) {
                const auto resource = index.get<uint32_t>();
                const auto &b = vk.buffers.at(allocations.at(resource_names.at(resource)));
                infos.push_back({b.buffer, 0,
                                 layout.at("buffers")
                                     .at(resource)
                                     .at("descriptor_size_bytes")
                                     .get<uint64_t>()});
            }
        } else {
            const auto bytes = kind == 48
                                   ? encode(layout.at("flattened_srt").get<std::vector<uint32_t>>())
                                   : shader_data_bytes;
            require(!bytes.empty() && bytes.size() <= limits.maxStorageBufferRange,
                    "internal shader data extent unsupported");
            const auto slot = vk.buffer(physical, bytes);
            infos.push_back({vk.buffers[slot].buffer, 0, bytes.size()});
        }
        require(infos.size() == count, "compiler descriptor count mismatch");
        total_descriptors += count;
    }
    require(total_descriptors <= limits.maxPerStageDescriptorStorageBuffers &&
                total_descriptors <= limits.maxDescriptorSetStorageBuffers,
            "descriptor count exceeds queried device limit");
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = uint32_t(bindings.size());
    set_info.pBindings = bindings.data();
    checked(vk.CreateDescriptorSetLayout(vk.device, &set_info, nullptr, &vk.set_layout),
            "vkCreateDescriptorSetLayout");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    const bool push_data = layout.at("shader_data").at("location") == "push_constants";
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &vk.set_layout;
    pipeline_layout.pushConstantRangeCount = push_data ? 1 : 0;
    pipeline_layout.pPushConstantRanges = &push;
    checked(vk.CreatePipelineLayout(vk.device, &pipeline_layout, nullptr, &vk.pipeline_layout),
            "vkCreatePipelineLayout");
    std::vector<uint32_t> module_words(spirv.size() / 4);
    std::memcpy(module_words.data(), spirv.data(), spirv.size());
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = spirv.size();
    module.pCode = module_words.data();
    checked(vk.CreateShaderModule(vk.device, &module, nullptr, &vk.module), "vkCreateShaderModule");
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required_size{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    required_size.requiredSubgroupSize = subgroup;
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline.stage.pNext = controlled ? &required_size : nullptr;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = vk.module;
    pipeline.stage.pName = "main";
    pipeline.layout = vk.pipeline_layout;
    checked(
        vk.CreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &vk.pipeline),
        "vkCreateComputePipelines");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   std::max(1u, total_descriptors)};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    checked(vk.CreateDescriptorPool(vk.device, &pool, nullptr, &vk.descriptor_pool),
            "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo allocate_set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate_set.descriptorPool = vk.descriptor_pool;
    allocate_set.descriptorSetCount = 1;
    allocate_set.pSetLayouts = &vk.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    checked(vk.AllocateDescriptorSets(vk.device, &allocate_set, &set), "vkAllocateDescriptorSets");
    std::vector<VkWriteDescriptorSet> writes;
    for (size_t i = 0; i < bindings.size(); ++i) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = bindings[i].binding;
        write.descriptorCount = bindings[i].descriptorCount;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = buffer_infos[i].data();
        writes.push_back(write);
    }
    vk.UpdateDescriptorSets(vk.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commands.queueFamilyIndex = family;
    checked(vk.CreateCommandPool(vk.device, &commands, nullptr, &vk.command_pool),
            "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate_commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_commands.commandPool = vk.command_pool;
    allocate_commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_commands.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    checked(vk.AllocateCommandBuffers(vk.device, &allocate_commands, &command),
            "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(vk.BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    vk.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, vk.pipeline);
    vk.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, vk.pipeline_layout, 0, 1,
                             &set, 0, nullptr);
    if (push_data) {
        const auto start = layout.at("shader_data").at("push_data_start_dword").get<uint32_t>();
        require(start <= 32 && shader_data.size() <= 32 - start && !shader_data.empty(),
                "invalid push-data extent");
        vk.CmdPushConstants(command, vk.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, start * 4,
                            uint32_t(shader_data_bytes.size()), shader_data_bytes.data());
    }
    vk.CmdDispatch(command, dispatch[0], dispatch[1], dispatch[2]);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 1, &barrier, 0, nullptr, 0, nullptr);
    checked(vk.EndCommandBuffer(command), "vkEndCommandBuffer");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    checked(vk.CreateFence(vk.device, &fence_info, nullptr, &vk.fence), "vkCreateFence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    checked(vk.QueueSubmit(queue, 1, &submit, vk.fence), "vkQueueSubmit");
    const auto waited = vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 10000000000ull);
    if (waited != VK_SUCCESS) {
        std::cerr << "Vulkan fence wait failed (" << waited
                  << "); terminating isolated worker without destroying pending resources\n"
                  << std::flush;
        std::_Exit(4); // Parent records backend_error; process death is not a GPU reset guarantee.
    }
    VulkanReplayResult result;
    for (const auto &[name, slot] : allocations) {
        const auto &b = vk.buffers[slot];
        if (inputs.resources.at(name).at("access") != "read_only") {
            auto &bytes = result.outputs[name];
            bytes.resize(b.size);
            std::memcpy(bytes.data(), b.mapped, b.size);
        }
    }
    result.trace = {{"schema", 1},
                    {"fixture", inputs.identity},
                    {"compiler", layout.at("identity")},
                    {"device_name", properties.properties.deviceName},
                    {"device_type", uint32_t(properties.properties.deviceType)},
                    {"vendor_id", properties.properties.vendorID},
                    {"device_id", properties.properties.deviceID},
                    {"driver_version", properties.properties.driverVersion},
                    {"api_version", properties.properties.apiVersion},
                    {"compiler_subgroup_size", subgroup},
                    {"subgroup_size", controlled ? subgroup : groups.subgroupSize},
                    {"subgroup_dependent_module", subgroup_dependent},
                    {"required_subgroup_size", controlled},
                    {"queue_family", family},
                    {"host_profile_source", "queried_from_selected_Vulkan_device"},
                    {"enabled_host", host},
                    {"module_requirements", assessment},
                    {"dispatch", dispatch},
                    {"shader_data_words", shader_data},
                    {"resource_mapping", json::array()},
                    {"execution", "Vulkan_compute_dispatch_and_fence_readback"},
                    {"semantic_correctness", "requires_independent_output_comparison"}};
    for (const auto &[index, name] : resource_names)
        result.trace["resource_mapping"].push_back(
            {{"compiler_resource", index}, {"fixture_resource", name}, {"host_offset", 0}});
    return result;
}
} // namespace sl
