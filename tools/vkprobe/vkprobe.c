// What the renderer asks the Vulkan driver (device.c, exec.c, foveation.c) in one list: device type, driver, limits, subgroup
// and shading-rate properties, features, formats, memory types, queue families, extensions. tools/vkprobe/run.sh builds
// it for the machine, or for the Steam Frame's Turnip under qemu (--frame).
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) { puts("no instance"); return 1; }
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
    printf("device %s vendor %#x type %d api %u.%u.%u\n", p.deviceName, p.vendorID, p.deviceType, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    printf("maxStorageBufferRange %u maxPerStageStorage %u maxSetStorage %u maxPushConstants %u\n", p.limits.maxStorageBufferRange, p.limits.maxPerStageDescriptorStorageBuffers, p.limits.maxDescriptorSetStorageBuffers, p.limits.maxPushConstantsSize);
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &drv};
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsrp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR, .pNext = &sg};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &fsrp};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("driver id %d name %s info %s\n", drv.driverID, drv.driverName, drv.driverInfo);
    printf("subgroup size %u stages %#x ops %#x\n", sg.subgroupSize, sg.supportedStages, sg.supportedOperations);
    printf("fsr texel min %ux%u max %ux%u layered %d nontrivialcombiner %d maxFragmentSize %ux%u\n", fsrp.minFragmentShadingRateAttachmentTexelSize.width, fsrp.minFragmentShadingRateAttachmentTexelSize.height, fsrp.maxFragmentShadingRateAttachmentTexelSize.width, fsrp.maxFragmentShadingRateAttachmentTexelSize.height, fsrp.layeredShadingRateAttachments, fsrp.fragmentShadingRateNonTrivialCombinerOps, fsrp.maxFragmentSize.width, fsrp.maxFragmentSize.height);
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsrf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, .pNext = &f11};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fsrf};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    printf("features: multiview %d int8 %d 8bitStorage %d scalarBlock %d bda %d sparseBinding %d sparseResBuf %d int64 %d fragStores %d vtxStores %d robust %d fsr pipeline %d attachment %d primitive %d\n", f11.multiview, f12.shaderInt8, f12.storageBuffer8BitAccess, f12.scalarBlockLayout, f12.bufferDeviceAddress, f2.features.sparseBinding, f2.features.sparseResidencyBuffer, f2.features.shaderInt64, f2.features.fragmentStoresAndAtomics, f2.features.vertexPipelineStoresAndAtomics, f2.features.robustBufferAccess, fsrf.pipelineFragmentShadingRate, fsrf.attachmentFragmentShadingRate, fsrf.primitiveFragmentShadingRate);
    VkFormat fm[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_R8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    for (int i = 0; i < 6; i++) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, fm[i], &fp); printf("format %d optimal %#x linear %#x\n", fm[i], fp.optimalTilingFeatures, fp.linearTilingFeatures); }
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) printf("heap %u size %llu MB flags %u\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) printf("type %u heap %u flags %#x\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL); VkQueueFamilyProperties q[8]; if (nq > 8) nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, q);
    for (uint32_t i = 0; i < nq; i++) printf("queue family %u flags %#x count %u tsbits %u\n", i, q[i].queueFlags, q[i].queueCount, q[i].timestampValidBits);
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL); VkExtensionProperties *e = calloc(ne, sizeof *e); vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, e);
    const char *want[] = {"VK_EXT_external_memory_host", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_map_memory_placed", "VK_KHR_map_memory2", "VK_EXT_custom_border_color", "VK_EXT_vertex_attribute_divisor", "VK_EXT_device_fault", "VK_EXT_memory_budget", "VK_EXT_vertex_input_dynamic_state", "VK_EXT_graphics_pipeline_library", "VK_EXT_extended_dynamic_state3", "VK_KHR_pipeline_binary", "VK_KHR_maintenance5", "VK_KHR_pipeline_executable_properties", "VK_EXT_fragment_density_map", "VK_KHR_fragment_shading_rate", "VK_KHR_multiview", "VK_KHR_swapchain"};
    for (unsigned w = 0; w < sizeof want / sizeof *want; w++) { int has = 0; for (uint32_t i = 0; i < ne; i++) if (!strcmp(e[i].extensionName, want[w])) has = 1; printf("ext %s %s\n", want[w], has ? "YES" : "no"); }
    return 0;
}
