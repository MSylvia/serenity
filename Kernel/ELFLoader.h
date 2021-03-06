#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include "ELFImage.h"

class ELFLoader {
public:
    explicit ELFLoader(const byte*);
    ~ELFLoader();

    bool load();
    Function<void*(LinearAddress, size_t, size_t, bool, bool, const String&)> alloc_section_hook;
    Function<void*(LinearAddress, size_t, size_t, size_t, bool, bool, const String&)> map_section_hook;
    char* symbol_ptr(const char* name);
    bool allocate_section(LinearAddress, size_t, size_t alignment, bool is_readable, bool is_writable);
    bool map_section(LinearAddress, size_t, size_t alignment, size_t offset_in_image, bool is_readable, bool is_writable);
    LinearAddress entry() const { return m_image.entry(); }

private:
    bool layout();
    bool perform_relocations();
    void* lookup(const ELFImage::Symbol&);
    char* area_for_section(const ELFImage::Section&);
    char* area_for_section_name(const char*);

    struct PtrAndSize {
        PtrAndSize() { }
        PtrAndSize(char* p, unsigned s)
            : ptr(p)
            , size(s)
        {
        }

        char* ptr { nullptr };
        unsigned size { 0 };
    };
    ELFImage m_image;

    HashMap<String, char*> m_sections;
};

