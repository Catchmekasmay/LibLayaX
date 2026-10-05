# The pinned ggml fetches its queues with vkGetDeviceQueue2 and a vk::DeviceQueueInfo2 whose
# padding bytes are not initialized. Wine 10.20 and later (11.0 included) look the queue up by
# comparing the whole structure byte by byte, so with non-zero padding they find no queue and
# return VK_NULL_HANDLE; the first vkQueueSubmit then faults inside Wine ("Assertion failed:
# !status && "vkQueueSubmit"" in winevulkan's loader_thunks.c). Real drivers read the fields only.
#
# A queue created without flags is fetched with vkGetDeviceQueue, which is also what the Vulkan
# specification asks for. A queue created with flags goes through vkGetDeviceQueue2 with a
# structure that is zeroed first. A missing queue is reported at load time.
laya_vk_replace(
  "    h->queue = device->device.getQueue2(queue_info2);"
  [=[    if (device->has_internally_synchronized_queues) {
        VkDeviceQueueInfo2 zeroed_info;
        memset(&zeroed_info, 0, sizeof(zeroed_info));
        zeroed_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
        zeroed_info.flags = static_cast<VkDeviceQueueCreateFlags>(queue_info2.flags);
        zeroed_info.queueFamilyIndex = queue_family_index;
        zeroed_info.queueIndex = queue_index;
        h->queue = device->device.getQueue2(*reinterpret_cast<const vk::DeviceQueueInfo2 *>(&zeroed_info));
    } else {
        h->queue = device->device.getQueue(queue_family_index, queue_index);
    }
    if (!h->queue) {
        throw std::runtime_error("The Vulkan driver returned no queue for family " + std::to_string(queue_family_index) +
                                 ", index " + std::to_string(queue_index));
    }]=])
