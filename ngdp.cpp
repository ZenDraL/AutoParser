#include "ngdp.h"
#include "http.h"
#include "path.h"
#include "checksum.h"
#include "logger.h"
#include <algorithm>

namespace NGDP {

  const std::string HOST = "http://us.patch.battle.net:1119";
  const std::string PROGRAM = "d3";
  const std::string CACHE = "cdncache";

  void preload(char const* task, File& file) {
    static uint8 buffer[1 << 18];
    size_t size = file.size();
    Logger::begin(size, task);
    while (file.tell() < size) {
      file.read(buffer, sizeof buffer);
      Logger::progress(file.tell(), false);
    }
    Logger::end();
    file.seek(0);
  }

  std::map<std::string, CdnData> GetCdns(std::string const& app) {
    std::map<std::string, CdnData> result;
    File f = HttpRequest::get(HOST + "/" + app + "/cdns");
    if (!f) return result;
    bool first = true;
    for (std::string const& line : f) {
      if (first) { first = false; continue; }
      auto parts = split(line, '|');
      auto& config = result[parts[0]];
      config.path = parts[1];
      config.hosts = split(parts[2], ' ');
    }
    return result;
  }
  std::map<std::string, VersionData> GetVersions(std::string const& app) {
    std::map<std::string, VersionData> result;
    File f = HttpRequest::get(HOST + "/" + app + "/versions");
    if (!f) return result;
    bool first = true;
    for (std::string const& line : f) {
      if (first) { first = false; continue; }
      auto parts = split(line, '|');
      auto& config = result[parts[0]];
      config.build = parts[1];
      config.cdn = parts[2];
      config.id = std::stoi(parts[4]);
      config.version = parts[5];
    }
    return result;
  }

  NGDP::NGDP(std::string const& app, std::string const& region) {
    auto cdns = GetCdns(app);
    auto versions = GetVersions(app);
    if (!cdns.count(region) || !versions.count(region)) {
      throw Exception("invalid region");
    }
    base_ = "http://" + cdns[region].hosts[0] + "/" + cdns[region].path + "/";
    version_ = versions[region];
    if (app == "d3") {
      auto v2 = GetVersions("d3t");
      if (v2.count(region)) {
        version_.build2 = v2[region].build;
      }
    }
  }

  std::string NGDP::geturl(std::string const& hash, std::string const& type, bool index) const {
    std::string url = base_ + type + "/" + hash.substr(0, 2) + "/" + hash.substr(2, 2) + "/" + hash;
    if (index) url += ".index";
    return url;
  }
  File NGDP::load(std::string const& hash, std::string const& type, bool index, char const* preload) const {
    std::string path = path::work() / CACHE / type / hash;
    if (index) path += ".index";
    File file(path);
    if (file) return file;
    file = HttpRequest::get(geturl(hash, type, index));
    if (!file) return file;
    if (preload) {
      ::NGDP::preload(preload, file);
    }
    File(path, "wb").copy(file);
    file.seek(0);
    return file;
  }

  File DecodeBLTE(File blte, uint32 eusize) {
    if (blte.read32(true) != 0x424c5445 /*BLTE*/) return File();
    uint32 headerSize = blte.read32(true);
    if (headerSize) {
      std::vector<uint32> csize;
      std::vector<uint32> usize;
      /*uint16 flags = */blte.read16(true);
      uint16 chunks = blte.read16(true);
      for (uint16 i = 0; i < chunks; ++i) {
        csize.push_back(blte.read32(true));
        usize.push_back(blte.read32(true));
        blte.seek(16, SEEK_CUR);
      }
      MemoryFile dst;
      std::vector<uint8> tmp;
      for (uint16 i = 0; i < chunks; ++i) {
        uint8 type = blte.read8();
        if (type == 'N') {
          if (csize[i] - 1 != usize[i]) return File();
          blte.read(dst.reserve(usize[i]), usize[i]);
        } else if (type == 'Z') {
          tmp.resize(csize[i] - 1);
          blte.read(&tmp[0], tmp.size());
          if (gzinflate(&tmp[0], tmp.size(), dst.reserve(usize[i]), &usize[i])) return File();
        } else {
          // unsupported compression
          return File();
        }
      }
      dst.seek(0);
      return dst;
    } else {
      uint8 type = blte.read8();
      uint64 offset = blte.tell();
      uint64 size = blte.size() - offset;
      if (type == 'N') {
        return blte.subfile(offset, size);
      } else if (type == 'Z' && eusize) {
        std::vector<uint8> tmp(size);
        blte.read(&tmp[0], size);
        MemoryFile dst;
        if (gzinflate(&tmp[0], size, dst.reserve(eusize), &eusize)) return File();
        dst.seek(0);
        return dst;
      } else {
        // unsupported compression
        return File();
      }
    }
  }

  std::map<std::string, std::string> ParseConfig(File file) {
    std::map<std::string, std::string> result;
    if (!file) return result;
    for (std::string const& line : file) {
      if (line[0] == '#') continue;
      size_t pos = line.find(" = ");
      if (pos == std::string::npos) continue;
      result[line.substr(0, pos)] = line.substr(pos + 3);
    }
    return result;
  }

#pragma pack(push, 1)
  struct EncodingFileHeader {
    uint16 signature;
    uint8 unk;
    uint8 sizeA;
    uint8 sizeB;
    uint16 flagsA;
    uint16 flagsB;
    uint32 entriesA;
    uint32 entriesB;
    uint8 unk2;
    uint32 stringSize;
  };
#pragma pack(pop)

  void from_string(Hash hash, std::string const& str) {
    int val;
    for (size_t i = 0; i < sizeof(Hash); ++i) {
      sscanf(&str[i * 2], "%02x", &val);
      hash[i] = val;
    }
  }
  std::string to_string(const Hash hash) {
    return MD5::format(hash);
  }

  Encoding::Encoding(File file) {
    EncodingFileHeader header;
    file.read(&header, sizeof header);
    flip(header.signature);
    flip(header.entriesA);
    flip(header.entriesB);
    flip(header.stringSize);
    if (header.signature != 0x454e /*EN*/ || header.sizeA != 16 || header.sizeB != 16) {
      throw Exception("invalid encoding file");
    }

    uint32 size = file.size();
    uint32 posHeaderA = sizeof(EncodingFileHeader) + header.stringSize;
    uint32 posEntriesA = posHeaderA + header.entriesA * 32;
    uint32 posHeaderB = posEntriesA + header.entriesA * 4096;
    uint32 posEntriesB = posHeaderB + header.entriesB * 32;
    uint32 posLayout = posEntriesB + header.entriesB * 4096;

    data_.resize(header.stringSize + (header.entriesA + header.entriesB) * 4096 + (size - posLayout));
    char* layouts = (char*) &data_[0];
    uint8* entriesA = &data_[header.stringSize];
    uint8* entriesB = entriesA + header.entriesA * 4096;
    layout_ = (char*) (entriesB + header.entriesB * 4096);

    file.read(layouts, header.stringSize);
    file.seek(posEntriesA, SEEK_SET);
    file.read(entriesA, header.entriesA * 4096);
    file.seek(posEntriesB, SEEK_SET);
    file.read(entriesB, header.entriesB * 4096);
    file.read(layout_, size - posLayout);

    for (char* ptr = layouts; ptr < layouts + header.stringSize; ++ptr) {
      layouts_.push_back(ptr);
      while (*ptr) ++ptr;
    }

    file.seek(posHeaderA, SEEK_SET);
    encodingTable_.resize(header.entriesA);
    for (uint32 i = 0; i < header.entriesA; ++i) {
      file.read(encodingTable_[i].hash, sizeof(Hash));
      Hash blockHash, realHash;
      file.read(blockHash, sizeof blockHash);
      MD5::checksum(entriesA, 4096, realHash);
      if (memcmp(realHash, blockHash, sizeof(Hash))) {
        throw Exception("encoding file checksum mismatch");
      }
      for (uint8* ptr = entriesA; ptr + sizeof(EncodingEntry) <= entriesA + 4096;) {
        EncodingEntry* entry = reinterpret_cast<EncodingEntry*>(ptr);
        if (!entry->keyCount) break;
        encodingTable_[i].entries.push_back(entry);
        flip(entry->usize);
        ptr += sizeof(EncodingEntry) + (entry->keyCount - 1) * sizeof(Hash);
      }
      entriesA += 4096;
    }

    Hash nilHash;
    memset(nilHash, 0, sizeof(Hash));

    file.seek(posHeaderB, SEEK_SET);
    layoutTable_.resize(header.entriesB);
    for (uint32 i = 0; i < header.entriesB; ++i) {
      file.read(layoutTable_[i].key, sizeof(Hash));
      Hash blockHash, realHash;
      file.read(blockHash, sizeof blockHash);
      MD5::checksum(entriesB, 4096, realHash);
      if (memcmp(realHash, blockHash, sizeof(Hash))) {
        throw Exception("encoding file checksum mismatch");
      }
      for (uint8* ptr = entriesB; ptr + sizeof(LayoutEntry) <= entriesB + 4096;) {
        LayoutEntry* entry = reinterpret_cast<LayoutEntry*>(ptr);
        if (!memcmp(entry->key, nilHash, sizeof(Hash))) {
          break;
        }
        layoutTable_[i].entries.push_back(entry);
        flip(entry->stringIndex);
        flip(entry->csize);
        ptr += sizeof(LayoutEntry);
      }
      entriesB += 4096;
    }
  }

  template<class Vec, class Comp>
  typename Vec::const_iterator find_hash(Vec const& vec, Comp less) {
    if (vec.empty() || less(vec[0])) return vec.end();
    size_t left = 0, right = vec.size();
    while (right - left > 1) {
      size_t mid = (left + right) / 2;
      if (less(vec[mid])) {
        right = mid;
      } else {
        left = mid;
      }
    }
    return vec.begin() + left;
  }

  Encoding::EncodingEntry const* Encoding::getEncoding(const Hash hash) const {
    auto it = find_hash(encodingTable_, [&hash](EncodingHeader const& rhs) {
      return memcmp(hash, rhs.hash, sizeof(Hash)) < 0;
    });
    if (it == encodingTable_.end()) return nullptr;
    auto sub = find_hash(it->entries, [&hash](EncodingEntry const* rhs) {
      return memcmp(hash, rhs->hash, sizeof(Hash)) < 0;
    });
    if (sub == it->entries.end()) return nullptr;
    if (memcmp((*sub)->hash, hash, sizeof(Hash))) return nullptr;
    return *sub;
  }
  Encoding::LayoutEntry const* Encoding::getLayout(const Hash key) const {
    auto it = find_hash(layoutTable_, [&key](LayoutHeader const& rhs) {
      return memcmp(key, rhs.key, sizeof(Hash)) < 0;
    });
    if (it == layoutTable_.end()) return nullptr;
    auto sub = find_hash(it->entries, [&key](LayoutEntry const* rhs) {
      return memcmp(key, rhs->key, sizeof(Hash)) < 0;
    });
    if (sub == it->entries.end()) return nullptr;
    if (memcmp((*sub)->key, key, sizeof(Hash))) return nullptr;
    return *sub;
  }

  ArchiveIndex::ArchiveIndex(NGDP const& ngdp, uint32 blockSize)
    : ngdp_(ngdp)
    , blockSize_(blockSize)
  {
    Hash nilHash;
    memset(nilHash, 0, sizeof(Hash));

    File cdnFile = ngdp.load(ngdp.version().cdn);
    if (!cdnFile) return;
    std::vector<std::string> archives = split(ParseConfig(cdnFile)["archives"]);

    archives_.resize(archives.size());
    Logger::begin(archives.size(), "Loading indices");
    for (size_t i = 0; i < archives.size(); ++i) {
      archives_[i].name = archives[i];

      Logger::item(nullptr);
      File index = ngdp.load(archives[i], "data", true);
      if (!index) continue;
      size_t size = index.size();
      for (size_t block = 0; block + 4096 <= size; block += 4096) {
        Hash hash;
        index.seek(block, SEEK_SET);
        for (size_t pos = 0; pos + 24 <= 4096; pos += 24) {
          index.read(hash, sizeof(Hash));
          if (!memcmp(hash, nilHash, sizeof(Hash))) {
            block = size;
            break;
          }
          auto& dst = index_[Hash_container::from(hash)];
          dst.index = i;
          dst.size = index.read32(true);
          dst.offset = index.read32(true);
        }
      }

      File map(path::work() / CACHE / "data" / archives[i] + ".map");
      if (map) {
        archives_[i].map.resize(map.size() / sizeof(uint32));
        map.read(&archives_[i].map[0], archives_[i].map.size() * sizeof(uint32));
      }
    }
    Logger::end();
  }

  File ArchiveIndex::load(Hash const& hash) {
    auto it = index_.find(Hash_container::from(hash));
    if (it == index_.end()) return ngdp_.load(hash, "data");
    std::string archivePath = path::work() / CACHE / "data" / archives_[it->second.index].name;

    uint32 offset = it->second.offset;
    uint32 size = it->second.size;
    uint32 blockStart = offset / blockSize_;
    uint32 blockEnd = (offset + size + blockSize_ - 1) / blockSize_;
    auto& map = archives_[it->second.index].map;
    uint32 getStart = blockEnd;
    uint32 getEnd = blockStart;
    for (uint32 i = blockStart; i < blockEnd; ++i) {
      if (i >= map.size() || map[i] == ArchiveInfo::INVALID) {
        getStart = std::min(getStart, i);
        getEnd = std::max(getEnd, i + 1);
      }
    }
    if (getStart < getEnd) {
      std::string url = ngdp_.geturl(archives_[it->second.index].name, "data");
      HttpRequest request(url);
      request.addHeader(fmtstring("Range: bytes=%u-%u", getStart * blockSize_, getEnd * blockSize_ - 1));
      request.send();
      uint32 status = request.status();
      if (status != 200 && status != 206) return File();
      auto headers = request.headers();
      File result = request.response();
      File archive = File(archivePath, "rb+");
      if (!archive) archive = File(archivePath, "wb+");
      if (headers.count("Content-Range")) {
        uint32 start, end, total, bstart, bend;
        if (sscanf(headers["Content-Range"].c_str(), "bytes %u-%u/%u", &start, &end, &total) != 3) {
          return File();
        }
        bstart = (start + blockSize_ - 1) / blockSize_;
        if (end >= total - 1) {
          bend = (total + blockSize_ - 1) / blockSize_;
        } else {
          bend = (end + 1) / blockSize_;
        }
        map.resize((total + blockSize_ - 1) / blockSize_, ArchiveInfo::INVALID);
        size_t filepos = archive.size();
        archive.seek(filepos, SEEK_SET);
        for (size_t i = bstart; i < bend; ++i) {
          if (map[i] == ArchiveInfo::INVALID) {
            map[i] = filepos;
            size_t size = std::min<size_t>(blockSize_, total - i * blockSize_);
            result.seek(i * blockSize_ - start, SEEK_SET);
            archive.copy(result, size);
            filepos += size;
          }
        }
        File(path::work() / CACHE / "data" / archives_[it->second.index].name + ".map", "wb").write(&map[0], map.size() * sizeof(uint32));
      } else {
        archive.copy(result);
        size_t size = result.size();
        size_t blocks = (size + blockSize_ - 1) / blockSize_;
        map.resize(blocks);
        for (size_t i = 0; i < blocks; ++i) {
          map[i] = i * blockSize_;
        }
        File(path::work() / CACHE / "data" / archives_[it->second.index].name + ".map", "wb").write(&map[0], map.size() * sizeof(uint32));
      }
    }

    File data(archivePath);
    if (!data) return File();
    MemoryFile result;
    uint8* output = result.reserve(size);
    for (uint32 i = blockStart; i < blockEnd; ++i) {
      if (map[i] == ArchiveInfo::INVALID) continue;
      uint32 curstart = std::max<uint32>(offset, i * blockSize_);
      uint32 curend = std::min<uint32>(offset + size, i * blockSize_ + blockSize_);
      data.seek(map[i] + curstart - i * blockSize_, SEEK_SET);
      data.read(output + curstart - offset, curend - curstart);
    }
    result.seek(0);
    return result;
  }

}
