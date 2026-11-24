#include "vkcommon.h"
int main(int argc,char**argv){ int t1=atoi(argv[1]), t2=atoi(argv[2]); unsigned mb=16; vkup();
  PFN_vkCreateDevice cd=(void*)gipa(inst,"vkCreateDevice");
  float pr=1; VkDeviceQueueCreateInfo q={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,0,0,0,1,&pr};
  VkDeviceCreateInfo dc={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,0,0,1,&q};
  VkDevice d; if(cd(pd,&dc,0,&d)){printf("cd fail\n");return 1;}
  PFN_vkAllocateMemory am=(void*)gipa(inst,"vkAllocateMemory");
  unsigned long long a1=0,a2=0,cap=12288; VkDeviceMemory m; VkResult r=0;
  while(a1+a2<cap){VkMemoryAllocateInfo a={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,(VkDeviceSize)mb<<20,t1}; r=am(d,&a,0,&m); if(r) break; a1+=mb;}
  printf("type %d: %llu MB then %d\n",t1,a1,r);
  while(a1+a2<cap){VkMemoryAllocateInfo a={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,(VkDeviceSize)mb<<20,t2}; r=am(d,&a,0,&m); if(r) break; a2+=mb;}
  printf("type %d: %llu MB then %d; total %llu MB\n",t2,a2,r,a1+a2);
  return 0;}
