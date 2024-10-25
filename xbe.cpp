#ifdef IDALDR
#include <ida.hpp>
#include <diskio.hpp>
#include <typeinf.hpp>
#else
#include <cstdarg>
#endif
#include <array>

#include "xbe.hpp"

// XOR keys comes from first 4 bytes of public key used to verify the image signature
std::array<uint32_t, 3> kKernelThunkXORKeys = {
  0x5B6D40B6, // Retail
  0xEFB1F152, // Debug
  0x46437DCD  // Beta
};
std::array<uint32_t, 3> kEntryPointXORKeys = {
  0xA8FC57AB, // Retail
  0x94859D4B, // Debug
  0xE682F45B, // Beta
};

bool XBEFile::load(void* file)
{
  seek(file, 0, SEEK_END);
  image_length_ = tell(file);
  seek(file, 0, SEEK_SET);

  sections_.clear();
  load_error_ = uint32_t(XBELoadError::Unfinished);

  // Read XBE header
  read(&xbe_header_, sizeof(xbe::XbeHeader), 1, file);
  if (xbe_header_.Signature != MAGIC_XBEH)
  {
    load_error_ = uint32_t(XBELoadError::InvalidMagic);
    return false;
  }

  // Read header bytes
  seek(file, 0, SEEK_SET);
  header_data_.resize(xbe_header_.SizeOfHeaders);
  read(header_data_.data(), 1, xbe_header_.SizeOfHeaders, file);

  // Read XBE sections
  uint32_t sections_begin = xbe_va_to_offset(xbe_header_.SectionHeadersOffset);
  seek(file, sections_begin, SEEK_SET);
  for (uint32_t i = 0; i < xbe_header_.NumberOfSections; i++)
  {
    XBESection section;
    read(&section.Info, sizeof(xbe::XbeSection), 1, file);
    auto pos = tell(file);

    // Read section name
    seek(file, xbe_va_to_offset(section.Info.SectionNameOffset), SEEK_SET);
    section.Name = read_null_terminated(file, 64);

    // Read section data
    size_t section_size = section.Info.VirtualSize;

    // If virtual size is beyond file bounds, resize it to what we can fit
    // TODO: should probably check against offset of other sections too, so we don't include their data
    if (section.Info.PointerToRawData + section_size > image_length_)
      section_size = image_length_ - section.Info.PointerToRawData;

    section.Data.resize(section_size);
    seek(file, section.Info.PointerToRawData, SEEK_SET);
    read(section.Data.data(), 1, section_size, file);

    sections_.push_back(section);

    // Seek back to end of XbeSection
    seek(file, pos, SEEK_SET);
  }

  // Read library info
  libraries_.clear();
  if (xbe_header_.LibraryVersionsOffset && xbe_header_.NumberOfLibraryVersions)
  {
    auto lib_versions_offset = xbe_va_to_offset(xbe_header_.LibraryVersionsOffset);
    if (image_length_ > lib_versions_offset)
    {
      seek(file, lib_versions_offset, SEEK_SET);
      for (uint32_t i = 0; i < xbe_header_.NumberOfLibraryVersions; i++)
      {
        xbe::XbeLibraryVersion version;
        read(&version, sizeof(xbe::XbeLibraryVersion), 1, file);
        libraries_.push_back(version);
      }
    }
  }

  // Read PE name
  if (xbe_header_.DebugPathNameOffset)
  {
    auto path_offset = xbe_va_to_offset(xbe_header_.DebugPathNameOffset);
    if (image_length_ > path_offset)
    {
      seek(file, path_offset, SEEK_SET);
      pe_module_name_ = read_null_terminated(file, 256);
    }
  }

  // Read in callbacks from TLS directory if exists
  if (xbe_header_.TlsDirectoryOffset)
  {
    auto tls_dir_offset = xbe_va_to_offset(xbe_header_.TlsDirectoryOffset);
    if (image_length_ > tls_dir_offset)
    {
      tls_directory_va_ = xbe_header_.TlsDirectoryOffset;
      seek(file, tls_dir_offset, 0);
      read(&tls_directory_, sizeof(IMAGE_TLS_DIRECTORY32), 1, file);
      if (tls_directory_.AddressOfCallBacks)
      {
        auto callback_offset = xbe_va_to_offset(tls_directory_.AddressOfCallBacks);
        dbgmsg("[+] Reading TLS callbacks from 0x%X (directory: 0x%X)\n", tls_directory_.AddressOfCallBacks, tls_directory_va_);

        if (image_length_ >= (callback_offset + sizeof(uint32_t)))
        {
          seek(file, callback_offset, 0);
          uint32_t callback = 0;
          read(&callback, sizeof(uint32_t), 1, file);
          while (callback)
          {
            tls_callbacks_.push_back(callback);
            read(&callback, sizeof(uint32_t), 1, file);
          }
        }
      }
    }
  }

  // Read debug info
  // TODO: scan for codeview RSDS header in older XBEs which don't include codeview offset in headers?
  if (xbe_header_.has_codeview_offset() && xbe_header_.CodeViewDebugInfoOffset)
  {
    auto codeview_offset = xbe_va_to_offset(xbe_header_.CodeViewDebugInfoOffset);
    if (image_length_ > codeview_offset)
    {
      CV_INFO_PDB70 cv_info;
      seek(file, codeview_offset, 0);
      read(&cv_info, sizeof(CV_INFO_PDB70), 1, file);
      if (cv_info.CvSignature == CV_INFO_RSDS_SIGNATURE)
      {
        std::string cv_fname = read_null_terminated(file, 256);

        std::vector<uint8_t> cv_data;
        cv_data.resize(sizeof(CV_INFO_PDB70) + cv_fname.length() + 1);
        std::copy_n((uint8_t*)&cv_info, sizeof(CV_INFO_PDB70), cv_data.data());
        std::copy_n(cv_fname.c_str(), cv_fname.length(), cv_data.data() + sizeof(CV_INFO_PDB70));
        cv_data[sizeof(CV_INFO_PDB70) + cv_fname.length()] = 0;

        codeview_data_.push_back(cv_data);
      }
    }
  }

  // Try figuring out the XOR key to use
  xorkey_index = -1;
  for (int i = 0; i < 3; i++)
  {
    auto xorkey = kKernelThunkXORKeys[i];
    uint32_t decrypted_thunk = xbe_header_.XboxKernelThunkDataOffset ^ xorkey;

    // If decrypted_thunk is between baseaddr and sizeofimage it's likely valid
    // TODO: should probably verify that ordinals look good though
    if (decrypted_thunk >= xbe_header_.BaseAddress &&
      decrypted_thunk < (xbe_header_.BaseAddress + xbe_header_.SizeOfImage))
    {
      xorkey_index = i;
      xbe_header_.XboxKernelThunkDataOffset = decrypted_thunk;
    }
  }

  if (xorkey_index == -1)
  {
    load_error_ = uint32_t(XBELoadError::UnknownXORKey);
    return false;
  }

  uint32_t decrypted_ep = xbe_header_.AddressOfEntryPoint ^ kEntryPointXORKeys[xorkey_index];
  xbe_header_.AddressOfEntryPoint = decrypted_ep;

  // Read kernel imports
  {
    uint32_t kernel_thunk_offset = xbe_va_to_offset(xbe_header_.XboxKernelThunkDataOffset);
    if (image_length_ > kernel_thunk_offset)
    {
      seek(file, kernel_thunk_offset, 0);

      int num = 0;
      while (num < 400)
      {
        uint32_t ordinal = 0;
        read(&ordinal, sizeof(uint32_t), 1, file);
        if (!ordinal)
          break;

        if ((ordinal & IMAGE_ORDINAL_FLAG32) == IMAGE_ORDINAL_FLAG32)
        {
          kernel_imports_.insert({ xbe_header_.XboxKernelThunkDataOffset + (num * 4), IMAGE_ORDINAL32(ordinal) });
        }
        else
        {
          // TODO: error? these should always have ordinal flag
        }
        num++;
      }
    }
  }

  load_error_ = uint32_t(XBELoadError::Success);
  return true;
}

uint32_t XBEFile::xbe_va_to_offset(uint32_t va)
{
  if (va < xbe_header_.BaseAddress)
    return va;

  for (auto& section : sections_)
  {
    uint32_t section_end = section.Info.VirtualAddress + section.Info.VirtualSize;
    if (section.Info.VirtualAddress > va || va >= section_end)
      continue;
    return (va - section.Info.VirtualAddress) + section.Info.PointerToRawData;
  }
  return va - xbe_header_.BaseAddress;
}

std::string XBEFile::read_null_terminated(void* file, size_t maxlen) {
  std::string result;
  char ch;
  std::size_t count = 0;

  // Read one character at a time using fread, stopping at maxlen or null terminator
  while (count < maxlen && read(&ch, sizeof(char), 1, file) == 1) {
    if (ch == '\0') {
      break;
    }
    result += ch;
    ++count;
  }

  return result;
}

#ifdef IDALDR
// Shim function to allow using IDA's qlread function
size_t idaread_xbe(void* buffer, size_t element_size, size_t element_count, void* file)
{
  return qlread((linput_t*)file, buffer, element_size * element_count);
}

void XBEFile::use_ida_io()
{
  read = idaread_xbe;
  seek = (seek_fn)qlseek;
  tell = (tell_fn)qltell;
  dbgmsg = msg;
}
#endif