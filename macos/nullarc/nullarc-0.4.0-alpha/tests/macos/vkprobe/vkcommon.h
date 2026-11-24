#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
typedef PFN_vkVoidFunction (*GIPA)(VkInstance, const char*);
static GIPA gipa; static VkInstance inst; static VkPhysicalDevice pd;
static void vkup(void){
  void *h=dlopen("/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/nvmtl/libvulkan_radeon.dylib",RTLD_NOW);
  if(!h){printf("dlopen %s\n",dlerror());exit(1);}
  gipa=(GIPA)dlsym(h,"vk_icdGetInstanceProcAddr");
  PFN_vkCreateInstance ci=(void*)gipa(NULL,"vkCreateInstance");
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO,0,"x",0,"x",0,VK_API_VERSION_1_2};
  VkInstanceCreateInfo ic={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,0,0,&ai};
  if(ci(&ic,0,&inst)){printf("ci fail\n");exit(1);}
  PFN_vkEnumeratePhysicalDevices ep=(void*)gipa(inst,"vkEnumeratePhysicalDevices");
  uint32_t n=1; ep(inst,&n,&pd);
}
