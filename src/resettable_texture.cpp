#include "lyrium/texture/level_validity.h"
#include "lyrium/resettable_texture.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

#include <windows.h>

#include "lyrium/allocators/global_allocator.h"
#include "lyrium/containers/map.h"
#include "lyrium/containers/unordered_map.h"
#include "lyrium/containers/unordered_set.h"
#include "lyrium/containers/vector.h"
#include "lyrium/diag/texture_size.h"

namespace lyrium
{

namespace
{

class ResettableTexture;

std::mutex registry_mutex;
UnorderedSet<ResettableTexture *> registered_textures;
UnorderedMap<::IDirect3DBaseTexture9 *, ResettableTexture *> textures_by_inner;

class ResettableSurface final : public ::IDirect3DSurface9
{
  public:
    ResettableSurface(ResettableTexture *texture, ::UINT level);

    auto STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) -> ::HRESULT override;
    auto STDMETHODCALLTYPE AddRef() -> ::ULONG override;
    auto STDMETHODCALLTYPE Release() -> ::ULONG override;
    auto STDMETHODCALLTYPE GetDevice(::IDirect3DDevice9 **device) -> ::HRESULT override;
    auto STDMETHODCALLTYPE SetPrivateData(
        REFGUID guid,
        const void *data,
        ::DWORD size,
        ::DWORD flags) -> ::HRESULT override;
    auto STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, ::DWORD *size) -> ::HRESULT override;
    auto STDMETHODCALLTYPE FreePrivateData(REFGUID guid) -> ::HRESULT override;
    auto STDMETHODCALLTYPE SetPriority(::DWORD priority) -> ::DWORD override;
    auto STDMETHODCALLTYPE GetPriority() -> ::DWORD override;
    auto STDMETHODCALLTYPE PreLoad() -> void override;
    auto STDMETHODCALLTYPE GetType() -> ::D3DRESOURCETYPE override;
    auto STDMETHODCALLTYPE GetContainer(REFIID iid, void **container) -> ::HRESULT override;
    auto STDMETHODCALLTYPE GetDesc(::D3DSURFACE_DESC *desc) -> ::HRESULT override;
    auto STDMETHODCALLTYPE LockRect(
        ::D3DLOCKED_RECT *locked,
        const ::RECT *rect,
        ::DWORD flags) -> ::HRESULT override;
    auto STDMETHODCALLTYPE UnlockRect() -> ::HRESULT override;
    auto STDMETHODCALLTYPE GetDC(::HDC *dc) -> ::HRESULT override;
    auto STDMETHODCALLTYPE ReleaseDC(::HDC dc) -> ::HRESULT override;

    static auto operator new(std::size_t size) -> void *
    {
        return GlobalAllocator::allocate(size);
    }

    static auto operator delete(void *allocation) noexcept -> void
    {
        GlobalAllocator::deallocate(allocation);
    }

  private:
    ~ResettableSurface();

    auto surface() -> ::IDirect3DSurface9 *;

    std::atomic<::ULONG> references_{1u};
    ResettableTexture *texture_;
    ::UINT level_;
};

class ResettableTexture final : public ::IDirect3DTexture9
{
  public:
    static auto create(
        ::IDirect3DDevice9 *device,
        CreateTextureFn create_texture,
        ::IDirect3DTexture9 *texture,
        const ResettableTextureShape &shape,
        TextureDestroyedFn destroyed) -> ResettableTexture *
    {
        try
        {
            auto *result = new ResettableTexture{device, create_texture, texture, shape, destroyed};
            if (result->mapping_ == nullptr)
            {
                delete result;
                return nullptr;
            }
            {
                auto lock = std::scoped_lock{registry_mutex};
                registered_textures.insert(result);
                textures_by_inner[texture] = result;
            }
            return result;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    auto STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) -> ::HRESULT override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (::IsEqualIID(iid, __uuidof(IUnknown)) ||
            ::IsEqualIID(iid, __uuidof(IDirect3DResource9)) ||
            ::IsEqualIID(iid, __uuidof(IDirect3DBaseTexture9)) ||
            ::IsEqualIID(iid, __uuidof(IDirect3DTexture9)))
        {
            *object = static_cast<::IDirect3DTexture9 *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    auto STDMETHODCALLTYPE AddRef() -> ::ULONG override
    {
        return references_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    auto STDMETHODCALLTYPE Release() -> ::ULONG override
    {
        const auto remaining = references_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (remaining == 0u)
        {
            delete this;
        }
        return remaining;
    }

    auto STDMETHODCALLTYPE GetDevice(::IDirect3DDevice9 **device) -> ::HRESULT override
    {
        if (device == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        *device = device_;
        device_->AddRef();
        return D3D_OK;
    }

    auto STDMETHODCALLTYPE SetPrivateData(
        REFGUID guid,
        const void *data,
        ::DWORD size,
        ::DWORD flags) -> ::HRESULT override
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        const auto result = texture->SetPrivateData(guid, data, size, flags);
        texture->Release();
        return result;
    }

    auto STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, ::DWORD *size) -> ::HRESULT override
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        const auto result = texture->GetPrivateData(guid, data, size);
        texture->Release();
        return result;
    }

    auto STDMETHODCALLTYPE FreePrivateData(REFGUID guid) -> ::HRESULT override
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        const auto result = texture->FreePrivateData(guid);
        texture->Release();
        return result;
    }

    auto STDMETHODCALLTYPE SetPriority(::DWORD priority) -> ::DWORD override
    {
        const auto previous = priority_.exchange(priority, std::memory_order_relaxed);
        auto *texture = acquire_inner();
        if (texture != nullptr)
        {
            texture->SetPriority(priority);
            texture->Release();
        }
        return previous;
    }

    auto STDMETHODCALLTYPE GetPriority() -> ::DWORD override
    {
        return priority_.load(std::memory_order_relaxed);
    }

    auto STDMETHODCALLTYPE PreLoad() -> void override
    {
        auto *texture = acquire_inner();
        if (texture != nullptr)
        {
            texture->PreLoad();
            texture->Release();
        }
    }

    auto STDMETHODCALLTYPE GetType() -> ::D3DRESOURCETYPE override
    {
        return D3DRTYPE_TEXTURE;
    }

    auto STDMETHODCALLTYPE SetLOD(::DWORD lod) -> ::DWORD override
    {
        return lod_.exchange(lod, std::memory_order_relaxed);
    }

    auto STDMETHODCALLTYPE GetLOD() -> ::DWORD override
    {
        return lod_.load(std::memory_order_relaxed);
    }

    auto STDMETHODCALLTYPE GetLevelCount() -> ::DWORD override
    {
        return shape_.levels;
    }

    auto STDMETHODCALLTYPE SetAutoGenFilterType(::D3DTEXTUREFILTERTYPE type) -> ::HRESULT override
    {
        filter_.store(type, std::memory_order_relaxed);
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        const auto result = texture->SetAutoGenFilterType(type);
        texture->Release();
        return result;
    }

    auto STDMETHODCALLTYPE GetAutoGenFilterType() -> ::D3DTEXTUREFILTERTYPE override
    {
        return filter_.load(std::memory_order_relaxed);
    }

    auto STDMETHODCALLTYPE GenerateMipSubLevels() -> void override
    {
        auto *texture = acquire_inner();
        if (texture != nullptr)
        {
            texture->GenerateMipSubLevels();
            texture->Release();
        }
    }

    auto STDMETHODCALLTYPE GetLevelDesc(::UINT level, ::D3DSURFACE_DESC *desc) -> ::HRESULT override
    {
        if (desc == nullptr || level >= layouts_.size())
        {
            return D3DERR_INVALIDCALL;
        }
        const auto &layout = layouts_[level];
        *desc = ::D3DSURFACE_DESC{
            .Format = shape_.format,
            .Type = D3DRTYPE_SURFACE,
            .Usage = shape_.usage,
            .Pool = D3DPOOL_MANAGED,
            .MultiSampleType = D3DMULTISAMPLE_NONE,
            .MultiSampleQuality = 0u,
            .Width = layout.width,
            .Height = layout.height,
        };
        return D3D_OK;
    }

    auto STDMETHODCALLTYPE GetSurfaceLevel(::UINT level, ::IDirect3DSurface9 **surface) -> ::HRESULT override
    {
        if (surface == nullptr || level >= layouts_.size())
        {
            return D3DERR_INVALIDCALL;
        }
        try
        {
            *surface = new ResettableSurface{this, level};
            return D3D_OK;
        }
        catch (...)
        {
            *surface = nullptr;
            return E_OUTOFMEMORY;
        }
    }

    auto STDMETHODCALLTYPE LockRect(
        ::UINT level,
        ::D3DLOCKED_RECT *locked,
        const ::RECT *rect,
        ::DWORD flags) -> ::HRESULT override
    {
        if (locked == nullptr || level >= layouts_.size())
        {
            return D3DERR_INVALIDCALL;
        }

        const auto &layout = layouts_[level];
        if (rect != nullptr &&
            (rect->left < 0 || rect->top < 0 || rect->right <= rect->left ||
             rect->bottom <= rect->top ||
             static_cast<std::uint32_t>(rect->right) > layout.width ||
             static_cast<std::uint32_t>(rect->bottom) > layout.height))
        {
            return D3DERR_INVALIDCALL;
        }

        auto lock = std::scoped_lock{lock_mutex_};
        auto &state = locks_[level];
        if (state.view != nullptr)
        {
            return D3DERR_INVALIDCALL;
        }

        auto *view = static_cast<std::byte *>(
            ::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0u, 0u, static_cast<::SIZE_T>(mapping_size_)));
        if (view == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        auto x = std::uint32_t{};
        auto y = std::uint32_t{};
        if (rect != nullptr)
        {
            x = static_cast<std::uint32_t>(rect->left) / 4u;
            y = static_cast<std::uint32_t>(rect->top) / 4u;
        }

        state.view = view;
        state.flags = flags;
        locked->Pitch = static_cast<::INT>(layout.pitch);
        locked->pBits = view + layout.offset +
                        static_cast<std::size_t>(y) * layout.pitch +
                        static_cast<std::size_t>(x) * block_bytes_;
        return D3D_OK;
    }

    auto STDMETHODCALLTYPE UnlockRect(::UINT level) -> ::HRESULT override
    {
        if (level >= layouts_.size())
        {
            return D3DERR_INVALIDCALL;
        }

        void *view = nullptr;
        auto flags = ::DWORD{};
        {
            auto lock = std::scoped_lock{lock_mutex_};
            auto &state = locks_[level];
            if (state.view == nullptr)
            {
                return D3DERR_INVALIDCALL;
            }
            view = state.view;
            flags = state.flags;
            state.view = nullptr;
            state.flags = 0u;
        }

        auto result = D3D_OK;
        if ((flags & D3DLOCK_READONLY) == 0u)
        {
            valid_.mark_valid(level);
            result = upload_level(level, static_cast<const std::byte *>(view));
        }
        ::UnmapViewOfFile(view);
        return result;
    }

    auto STDMETHODCALLTYPE AddDirtyRect(const ::RECT *) -> ::HRESULT override
    {
        return D3D_OK;
    }

    auto acquire_inner() -> ::IDirect3DTexture9 *
    {
        if (!ensure_inner())
        {
            return nullptr;
        }
        auto lock = std::scoped_lock{inner_mutex_};
        if (inner_ != nullptr)
        {
            inner_->AddRef();
        }
        return inner_;
    }

    auto inner_for_binding() -> ::IDirect3DBaseTexture9 *
    {
        if (!ensure_inner())
        {
            return nullptr;
        }
        auto lock = std::scoped_lock{inner_mutex_};
        return inner_;
    }

    auto surface_for_level(::UINT level) -> ::IDirect3DSurface9 *
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return nullptr;
        }
        auto *surface = static_cast<::IDirect3DSurface9 *>(nullptr);
        texture->GetSurfaceLevel(level, &surface);
        texture->Release();
        return surface;
    }

    auto release_inner() -> void
    {
        auto *released = static_cast<::IDirect3DTexture9 *>(nullptr);
        {
            auto lock = std::scoped_lock{inner_mutex_};
            released = inner_;
            inner_ = nullptr;
        }
        if (released != nullptr)
        {
            {
                auto lock = std::scoped_lock{registry_mutex};
                textures_by_inner.erase(released);
            }
            released->Release();
        }
    }

    auto restore_inner() -> void
    {
        ensure_inner();
    }

    static auto operator new(std::size_t size) -> void *
    {
        return GlobalAllocator::allocate(size);
    }

    static auto operator delete(void *allocation) noexcept -> void
    {
        GlobalAllocator::deallocate(allocation);
    }

  private:
    struct Layout
    {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t pitch;
        std::uint32_t rows;
        std::size_t offset;
    };

    struct LockState
    {
        void *view;
        ::DWORD flags;
    };

    ResettableTexture(
        ::IDirect3DDevice9 *device,
        CreateTextureFn create_texture,
        ::IDirect3DTexture9 *texture,
        const ResettableTextureShape &shape,
        TextureDestroyedFn destroyed)
        : device_{device}
        , create_texture_{create_texture}
        , inner_{texture}
        , shape_{shape}
        , destroyed_{destroyed}
    {
        device_->AddRef();
        texture->AddRef();
        shape_.levels = texture->GetLevelCount();
        block_bytes_ = diag::format_bits_per_pixel(shape_.format) == 4u ? 8u : 16u;

        auto width = std::max(shape_.width, 1u);
        auto height = std::max(shape_.height, 1u);
        auto offset = std::size_t{};
        layouts_.reserve(shape_.levels);
        locks_.resize(shape_.levels, LockState{.view = nullptr, .flags = 0u});


        for (auto level = std::uint32_t{}; level < shape_.levels; ++level)
        {
            const auto blocks_x = (width + 3u) / 4u;
            const auto rows = (height + 3u) / 4u;
            const auto pitch = blocks_x * block_bytes_;
            layouts_.push_back(Layout{
                .width = width,
                .height = height,
                .pitch = pitch,
                .rows = rows,
                .offset = offset,
            });
            offset += static_cast<std::size_t>(pitch) * rows;
            width = std::max(width / 2u, 1u);
            height = std::max(height / 2u, 1u);
        }

        mapping_size_ = offset;
        if (mapping_size_ == 0u || mapping_size_ > std::numeric_limits<::SIZE_T>::max())
        {
            return;
        }
        const auto size = static_cast<std::uint64_t>(mapping_size_);
        mapping_ = ::CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            static_cast<::DWORD>(size >> 32u),
            static_cast<::DWORD>(size & 0xffffffffu),
            nullptr);
    }

    ~ResettableTexture()
    {
        {
            auto lock = std::scoped_lock{registry_mutex};
            registered_textures.erase(this);
            if (inner_ != nullptr)
            {
                textures_by_inner.erase(inner_);
            }
        }

        for (auto &state : locks_)
        {
            if (state.view != nullptr)
            {
                ::UnmapViewOfFile(state.view);
            }
        }
        if (inner_ != nullptr)
        {
            inner_->Release();
        }
        if (mapping_ != nullptr)
        {
            ::CloseHandle(mapping_);
        }
        device_->Release();
        if (destroyed_ != nullptr)
        {
            destroyed_(this);
        }
    }

    auto ensure_inner() -> bool
    {
        {
            auto lock = std::scoped_lock{inner_mutex_};
            if (inner_ != nullptr)
            {
                return true;
            }
        }

        auto *created = static_cast<::IDirect3DTexture9 *>(nullptr);
        const auto result = create_texture_(
            device_,
            shape_.width,
            shape_.height,
            shape_.levels,
            shape_.usage,
            shape_.format,
            D3DPOOL_DEFAULT,
            &created,
            nullptr);
        if (FAILED(result) || created == nullptr)
        {
            return false;
        }

        {
            auto lock = std::scoped_lock{inner_mutex_};
            if (inner_ != nullptr)
            {
                created->Release();
                return true;
            }
            inner_ = created;
        }
        {
            auto lock = std::scoped_lock{registry_mutex};
            textures_by_inner[created] = this;
        }

        created->SetPriority(priority_.load(std::memory_order_relaxed));
        created->SetAutoGenFilterType(filter_.load(std::memory_order_relaxed));
        upload_all();
        return true;
    }

    auto upload_level(::UINT level, const std::byte *view) -> ::HRESULT
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_DEVICELOST;
        }

        const auto &layout = layouts_[level];
        auto *staging = static_cast<::IDirect3DTexture9 *>(nullptr);
        auto result = create_texture_(
            device_,
            layout.width,
            layout.height,
            1u,
            0u,
            shape_.format,
            D3DPOOL_SYSTEMMEM,
            &staging,
            nullptr);
        if (SUCCEEDED(result) && staging != nullptr)
        {
            auto locked = ::D3DLOCKED_RECT{};
            result = staging->LockRect(0u, &locked, nullptr, 0u);
            if (SUCCEEDED(result))
            {
                const auto *source = view + layout.offset;
                auto *destination = static_cast<std::byte *>(locked.pBits);
                for (auto row = std::uint32_t{}; row < layout.rows; ++row)
                {
                    std::memcpy(
                        destination + static_cast<std::size_t>(row) * static_cast<std::size_t>(locked.Pitch),
                        source + static_cast<std::size_t>(row) * layout.pitch,
                        layout.pitch);
                }
                result = staging->UnlockRect(0u);
            }

            auto *source_surface = static_cast<::IDirect3DSurface9 *>(nullptr);
            auto *destination_surface = static_cast<::IDirect3DSurface9 *>(nullptr);
            if (SUCCEEDED(result))
            {
                result = staging->GetSurfaceLevel(0u, &source_surface);
            }
            if (SUCCEEDED(result))
            {
                result = texture->GetSurfaceLevel(level, &destination_surface);
            }
            if (SUCCEEDED(result))
            {
                result = device_->UpdateSurface(source_surface, nullptr, destination_surface, nullptr);
            }
            if (source_surface != nullptr)
            {
                source_surface->Release();
            }
            if (destination_surface != nullptr)
            {
                destination_surface->Release();
            }
            staging->Release();
        }
        texture->Release();
        return result;
    }

    auto upload_all() -> void
    {
        auto *view = static_cast<const std::byte *>(
            ::MapViewOfFile(mapping_, FILE_MAP_READ, 0u, 0u, static_cast<::SIZE_T>(mapping_size_)));
        if (view == nullptr)
        {
            return;
        }
        for (auto level = std::uint32_t{}; level < shape_.levels; ++level)
        {
            if (valid_.is_valid(level))
            {
                upload_level(level, view);
            }
        }
        ::UnmapViewOfFile(view);
    }

    std::atomic<::ULONG> references_{1u};
    ::IDirect3DDevice9 *device_;
    CreateTextureFn create_texture_;
    std::mutex inner_mutex_;
    ::IDirect3DTexture9 *inner_;
    ResettableTextureShape shape_;
    TextureDestroyedFn destroyed_;
    ::HANDLE mapping_{nullptr};
    std::size_t mapping_size_{0u};
    std::uint32_t block_bytes_{0u};
    Vector<Layout> layouts_;
    Vector<LockState> locks_;
    texture::LevelValidity valid_;
    std::mutex lock_mutex_;
    std::atomic<::DWORD> priority_{0u};
    std::atomic<::DWORD> lod_{0u};
    std::atomic<::D3DTEXTUREFILTERTYPE> filter_{D3DTEXF_POINT};
};

ResettableSurface::ResettableSurface(ResettableTexture *texture, ::UINT level)
    : texture_{texture}
    , level_{level}
{
    texture_->AddRef();
}

ResettableSurface::~ResettableSurface()
{
    texture_->Release();
}

auto STDMETHODCALLTYPE ResettableSurface::QueryInterface(REFIID iid, void **object) -> ::HRESULT
{
    if (object == nullptr)
    {
        return E_POINTER;
    }
    *object = nullptr;
    if (::IsEqualIID(iid, __uuidof(IUnknown)) ||
        ::IsEqualIID(iid, __uuidof(IDirect3DResource9)) ||
        ::IsEqualIID(iid, __uuidof(IDirect3DSurface9)))
    {
        *object = static_cast<::IDirect3DSurface9 *>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

auto STDMETHODCALLTYPE ResettableSurface::AddRef() -> ::ULONG
{
    return references_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

auto STDMETHODCALLTYPE ResettableSurface::Release() -> ::ULONG
{
    const auto remaining = references_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (remaining == 0u)
    {
        delete this;
    }
    return remaining;
}

auto ResettableSurface::surface() -> ::IDirect3DSurface9 *
{
    return texture_->surface_for_level(level_);
}

auto STDMETHODCALLTYPE ResettableSurface::GetDevice(::IDirect3DDevice9 **device) -> ::HRESULT
{
    return texture_->GetDevice(device);
}

auto STDMETHODCALLTYPE ResettableSurface::SetPrivateData(
    REFGUID guid,
    const void *data,
    ::DWORD size,
    ::DWORD flags) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->SetPrivateData(guid, data, size, flags);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::GetPrivateData(
    REFGUID guid,
    void *data,
    ::DWORD *size) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->GetPrivateData(guid, data, size);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::FreePrivateData(REFGUID guid) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->FreePrivateData(guid);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::SetPriority(::DWORD priority) -> ::DWORD
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return 0u;
    }
    const auto result = inner->SetPriority(priority);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::GetPriority() -> ::DWORD
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return 0u;
    }
    const auto result = inner->GetPriority();
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::PreLoad() -> void
{
    auto *inner = surface();
    if (inner != nullptr)
    {
        inner->PreLoad();
        inner->Release();
    }
}

auto STDMETHODCALLTYPE ResettableSurface::GetType() -> ::D3DRESOURCETYPE
{
    return D3DRTYPE_SURFACE;
}

auto STDMETHODCALLTYPE ResettableSurface::GetContainer(REFIID iid, void **container) -> ::HRESULT
{
    return texture_->QueryInterface(iid, container);
}

auto STDMETHODCALLTYPE ResettableSurface::GetDesc(::D3DSURFACE_DESC *desc) -> ::HRESULT
{
    return texture_->GetLevelDesc(level_, desc);
}

auto STDMETHODCALLTYPE ResettableSurface::LockRect(
    ::D3DLOCKED_RECT *locked,
    const ::RECT *rect,
    ::DWORD flags) -> ::HRESULT
{
    return texture_->LockRect(level_, locked, rect, flags);
}

auto STDMETHODCALLTYPE ResettableSurface::UnlockRect() -> ::HRESULT
{
    return texture_->UnlockRect(level_);
}

auto STDMETHODCALLTYPE ResettableSurface::GetDC(::HDC *dc) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->GetDC(dc);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::ReleaseDC(::HDC dc) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->ReleaseDC(dc);
    inner->Release();
    return result;
}

auto snapshot_textures() -> Vector<ResettableTexture *>
{
    auto result = Vector<ResettableTexture *>{};
    auto lock = std::scoped_lock{registry_mutex};
    result.reserve(registered_textures.size());
    for (auto *texture : registered_textures)
    {
        texture->AddRef();
        result.push_back(texture);
    }
    return result;
}

}

auto make_resettable_texture(
    ::IDirect3DDevice9 *device,
    CreateTextureFn create_texture,
    ::IDirect3DTexture9 *texture,
    const ResettableTextureShape &shape,
    TextureDestroyedFn destroyed) -> ::IDirect3DTexture9 *
{
    if (device == nullptr || create_texture == nullptr || texture == nullptr)
    {
        return nullptr;
    }
    return ResettableTexture::create(device, create_texture, texture, shape, destroyed);
}

auto unwrap_resettable_texture(::IDirect3DBaseTexture9 *texture) -> ::IDirect3DBaseTexture9 *
{
    if (texture == nullptr)
    {
        return nullptr;
    }

    auto *resettable = static_cast<ResettableTexture *>(nullptr);
    {
        auto lock = std::scoped_lock{registry_mutex};
        const auto found = registered_textures.find(reinterpret_cast<ResettableTexture *>(texture));
        if (found != registered_textures.end())
        {
            resettable = *found;
        }
    }
    return resettable != nullptr ? resettable->inner_for_binding() : texture;
}

auto rewrap_resettable_texture(::IDirect3DBaseTexture9 *texture) -> ::IDirect3DBaseTexture9 *
{
    if (texture == nullptr)
    {
        return nullptr;
    }

    auto *resettable = static_cast<ResettableTexture *>(nullptr);
    {
        auto lock = std::scoped_lock{registry_mutex};
        const auto found = textures_by_inner.find(texture);
        if (found != textures_by_inner.end())
        {
            resettable = found->second;
            resettable->AddRef();
        }
    }
    if (resettable == nullptr)
    {
        return texture;
    }
    texture->Release();
    return resettable;
}

auto before_resettable_texture_reset(::IDirect3DDevice9 *device) -> void
{
    if (device != nullptr)
    {
        for (auto stage = ::DWORD{}; stage < 16u; ++stage)
        {
            device->SetTexture(stage, nullptr);
        }
        device->SetTexture(D3DDMAPSAMPLER, nullptr);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER1, nullptr);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER2, nullptr);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER3, nullptr);
    }

    auto textures = snapshot_textures();
    for (auto *texture : textures)
    {
        texture->release_inner();
        texture->Release();
    }
}

auto after_resettable_texture_reset() -> void
{
    auto textures = snapshot_textures();
    for (auto *texture : textures)
    {
        texture->restore_inner();
        texture->Release();
    }
}

}
