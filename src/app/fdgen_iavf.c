#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vfio.h>
#include <firedancer/util/fd_util.h>

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  int container = open( "/dev/vfio/vfio", O_RDWR );
  FD_TEST( container>=0 );

  FD_TEST( ioctl( container, VFIO_GET_API_VERSION )==VFIO_API_VERSION );
  FD_TEST( ioctl( container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU ) );

  int group = open( "/dev/vfio/119", O_RDWR );
  FD_TEST( container>=0 );

  struct vfio_group_status group_status = { .argsz = sizeof(struct vfio_group_status) };
  FD_TEST( 0==ioctl( group, VFIO_GROUP_GET_STATUS, &group_status ) );
  FD_TEST( group_status.flags & VFIO_GROUP_FLAGS_VIABLE );
  FD_TEST( 0==ioctl( group, VFIO_GROUP_SET_CONTAINER, &container ) );
  FD_TEST( 0==ioctl( container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU ) );

  struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(struct vfio_iommu_type1_info) };
  FD_TEST( 0==ioctl( container, VFIO_IOMMU_GET_INFO, &iommu_info ) );

  struct vfio_iommu_type1_dma_map dma_map = { .argsz = sizeof(struct vfio_iommu_type1_dma_map) };
  dma_map.vaddr = (ulong)mmap( 0, 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0 );
  dma_map.size  = 1024 * 1024;
  dma_map.iova  = 0; /* 1MB starting at 0x0 from device view */
  dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
  FD_TEST( 0==ioctl( container, VFIO_IOMMU_MAP_DMA, &dma_map ) );

  int device = ioctl( group, VFIO_GROUP_GET_DEVICE_FD, "0000:90:01.0" );

  struct vfio_device_info device_info = { .argsz = sizeof(struct vfio_device_info) };
  FD_TEST( 0==ioctl( device, VFIO_DEVICE_GET_INFO, &device_info ) );

  ioctl( device, VFIO_DEVICE_RESET );

  close( group );
  close( container );

  fd_halt();
  return 0;
}
