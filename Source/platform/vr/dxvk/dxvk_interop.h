/**
 * @file dxvk_interop.h
 *
 * DXVK's Vulkan interop interfaces: the Vulkan objects behind a D3D11 device, and D3D11
 * textures made from existing Vulkan images. dxvk-native does not install this header, so
 * the declarations are copied from DXVK 3.1.1 (src/dxgi/dxgi_interfaces.h), which is
 * Copyright (c) 2017 Philip Rebohle and others, under the zlib/libpng license.
 */
#pragma once

#include <d3d11_3.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>

struct IDXGIVkInteropDevice;

MIDL_INTERFACE("5546cf8c-77e7-4341-b05d-8d4d5000e77d")
IDXGIVkInteropSurface : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE GetDevice(IDXGIVkInteropDevice **ppDevice) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage *pHandle, VkImageLayout *pLayout, VkImageCreateInfo *pInfo) = 0;
};

MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09323")
IDXGIVkInteropDevice : public IUnknown {
	virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *pInstance, VkPhysicalDevice *pPhysDev, VkDevice *pDevice) = 0;
	virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *pQueue, uint32_t *pQueueFamilyIndex) = 0;
	virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(IDXGIVkInteropSurface *pSurface, const VkImageSubresourceRange *pSubresources, VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
	/** Must be called before anyone else uses the queue on images D3D11 has drawn to. */
	virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
	/** No D3D11 call may be made from the locking thread until the queue is released. */
	virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
	virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};

MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09324")
IDXGIVkInteropDevice1 : public IDXGIVkInteropDevice {
	virtual void STDMETHODCALLTYPE GetSubmissionQueue1(VkQueue *pQueue, uint32_t *pQueueIndex, uint32_t *pQueueFamilyIndex) = 0;
	/** The texture does not own the image. */
	virtual HRESULT STDMETHODCALLTYPE CreateTexture2DFromVkImage(const D3D11_TEXTURE2D_DESC1 *pDesc, VkImage vkImage, ID3D11Texture2D **ppTexture2D) = 0;
};

MIDL_INTERFACE("3a6d8f2c-b0e8-4ab4-b4dc-4fd24891bfa5")
IDXGIVkInteropAdapter : public IUnknown {
	virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *pInstance, VkPhysicalDevice *pPhysDev) = 0;
};

MIDL_INTERFACE("4c5e1b0d-b0c8-4131-bfd8-9b2476f7f408")
IDXGIVkInteropFactory : public IUnknown {
	virtual void STDMETHODCALLTYPE GetVulkanInstance(VkInstance *pInstance, PFN_vkGetInstanceProcAddr *ppfnVkGetInstanceProcAddr) = 0;
};

#ifndef _MSC_VER
__CRT_UUID_DECL(IDXGIVkInteropSurface, 0x5546cf8c, 0x77e7, 0x4341, 0xb0, 0x5d, 0x8d, 0x4d, 0x50, 0x00, 0xe7, 0x7d);
__CRT_UUID_DECL(IDXGIVkInteropDevice, 0xe2ef5fa5, 0xdc21, 0x4af7, 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23);
__CRT_UUID_DECL(IDXGIVkInteropDevice1, 0xe2ef5fa5, 0xdc21, 0x4af7, 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x24);
__CRT_UUID_DECL(IDXGIVkInteropAdapter, 0x3a6d8f2c, 0xb0e8, 0x4ab4, 0xb4, 0xdc, 0x4f, 0xd2, 0x48, 0x91, 0xbf, 0xa5);
__CRT_UUID_DECL(IDXGIVkInteropFactory, 0x4c5e1b0d, 0xb0c8, 0x4131, 0xbf, 0xd8, 0x9b, 0x24, 0x76, 0xf7, 0xf4, 0x08);
#endif
