#include "vkcommon.h"
int main(void){ vkup();
  PFN_vkGetPhysicalDeviceMemoryProperties2 mp2=(void*)gipa(inst,"vkGetPhysicalDeviceMemoryProperties2");
  VkPhysicalDeviceMemoryBudgetPropertiesEXT b={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 m={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,&b};
  mp2(pd,&m);
  for(uint32_t i=0;i<m.memoryProperties.memoryHeapCount;i++) printf("heap %u size %llu MB flags %x budget %llu MB\n",i,
    m.memoryProperties.memoryHeaps[i].size>>20,m.memoryProperties.memoryHeaps[i].flags,b.heapBudget[i]>>20);
  return 0;}
