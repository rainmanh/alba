/*
Copyright (C) 2016 iNuron NV

This file is part of Open vStorage Open Source Edition (OSE), as available from


  http://www.openvstorage.org and
  http://www.openvstorage.com.

This file is free software; you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License v3 (GNU AGPLv3)
as published by the Free Software Foundation, in version 3 as it comes
in the <LICENSE.txt> file of the Open vStorage OSE distribution.

Open vStorage is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY of any kind.
*/

#include "proxy_protocol.h"
#include "llio.h"
#include "snappy.h"
#include <boost/optional/optional_io.hpp>
#include "stuff.h"

namespace alba {
namespace llio {

template <> void from(message &m, proxy_protocol::EncodingScheme &es) {
  uint8_t version;
  from(m, version);
  assert(version == 1);
  from(m, es.k);
  from(m, es.m);
  from(m, es.w);
}

template <> void from(message &m, proxy_protocol::Compression *&c) {
  uint8_t type;
  from(m, type);
  proxy_protocol::Compression *r;
  switch (type) {
  case 1: {
    r = new proxy_protocol::NoCompression();
  }; break;
  case 2: {
    r = new proxy_protocol::SnappyCompression();
  }; break;
  case 3: {
    r = new proxy_protocol::BZip2Compression();
  }; break;
  default: { throw "serialization error"; };
  }
  c = r;
}

template <> void from(message &m, proxy_protocol::EncryptInfo *&info) {
  uint8_t type;
  from(m, type);
  switch (type) {
  case 1: {
    info = new proxy_protocol::NoEncryption();
  }; break;
  default: { throw "serialization error"; };
  }
}

template <> void from(message &m, proxy_protocol::Manifest &mf) {
  uint8_t version;
  from(m, version);
  assert(version == 1);
  std::string compressed;
  from(m, compressed);

  std::string real;
  snappy::Uncompress(compressed.data(), compressed.size(), &real);
  std::vector<char> buffer(real.begin(), real.end());
  message m2(buffer);
  from(m2, mf.name);
  from(m2, mf.object_id);

  std::vector<uint32_t> chunk_sizes;
  from(m2, mf.chunk_sizes);

  uint8_t version2;
  from(m2, version2);
  assert(version2 == 1);
  from(m2, mf.encoding_scheme);
  from(m2, mf.compression);
  from(m2, mf.encrypt_info);
  from(m2, mf.checksum);
  from(m2, mf.size);
  uint8_t layout_tag;
  from(m2, layout_tag);
  assert(layout_tag == 1);
  from(m2, mf.fragment_locations);

  uint8_t layout_tag2;
  from(m2, layout_tag2);
  assert(layout_tag2 == 1);

  uint32_t n_chunks;
  from(m2, n_chunks);

  // TODO: how to this via the layout based template ?
  for (uint32_t i = 0; i < n_chunks; i++) {
    std::vector<std::shared_ptr<alba::Checksum>> chunk;
    uint32_t n_fragments;
    from(m2, n_fragments);
    for (uint32_t f = 0; f < n_fragments; f++) {
      alba::Checksum *p;
      from(m2, p);
      std::shared_ptr<alba::Checksum> sp(p);
      chunk.push_back(sp);
    };
    mf.fragment_checksums.push_back(chunk);
  }

  uint8_t layout_tag3;
  from(m2, layout_tag3);

  assert(layout_tag3 == 1);
  from(m2, mf.fragment_packed_sizes);

  from(m2, mf.version_id);
  from(m2, mf.max_disks_per_node);
  from(m2, mf.timestamp);
}
}
namespace proxy_protocol {

std::ostream &operator<<(std::ostream &os, const EncodingScheme &scheme) {
  os << "EncodingScheme{k=" << scheme.k << ", m=" << scheme.m
     << ", w=" << (int)scheme.w << "}";

  return os;
}

std::ostream &operator<<(std::ostream &os, const compressor_t &compressor) {
  switch (compressor) {
  case compressor_t::NO_COMPRESSION:
    os << "NO_COMPRESSION";
    break;
  case compressor_t::SNAPPY:
    os << "SNAPPY";
    break;
  case compressor_t::BZIP2:
    os << "BZIP2";
  };
  return os;
}
std::ostream &operator<<(std::ostream &os, const Compression &c) {
  c.print(os);
  return os;
}

std::ostream &operator<<(std::ostream &os, const encryption_t &encryption) {
  switch (encryption) {
  case encryption_t::NO_ENCRYPTION:
    os << "NO_ENCRYPTION";
    break;
  default:
    os << "?encryption?";
  };
  return os;
}

std::ostream &operator<<(std::ostream &os, const EncryptInfo &info) {
  info.print(os);
  return os;
}

std::ostream &operator<<(std::ostream &os, const fragment_location_t &f) {
  os << "(" << f.first // boost knows how
     << ", " << f.second << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const Manifest &mf) {
  os << "{"
     << "name = " << mf.name << "," << std::endl
     << "  object_id = `";

  const char *bytes = mf.object_id.data();
  const int bytes_size = mf.object_id.size();
  stuff::dump_buffer(os, bytes, bytes_size);

  os << "`, " << std::endl
     << "  chunk_sizes = ..."
     << "," << std::endl
     << "  encoding_scheme = " << mf.encoding_scheme << "," << std::endl
     << "  compression =..."
     << "," << std::endl
     << "  encryptinfo =" << *mf.encrypt_info << "," // dangerous
     << std::endl
     << "  checksum= " << *mf.checksum << "," << std::endl
     << "  size = " << mf.size << std::endl
     << "  fragment_locations = [" << std::endl;

  // TODO: why doesn't it do the right thing automatically?

  for (auto &t : mf.fragment_locations) {
    os << "  [";
    for (auto &fl : t) {
      os << fl << ", ";
    }
    os << "], " << std::endl;
  }

  os << "  ], " << std::endl
     << "  fragment_checksums = [" << std::endl;

  for (auto &c : mf.fragment_checksums) {
    os << "  [";
    for (const std::shared_ptr<alba::Checksum> &fc : c) {
      os << *fc << ", ";
    }
    os << "], " << std::endl;
  }
  os << "  ], ";

  os << std::endl
     << "  fragment_packed_sizes = [" << std::endl;

  // TODO: template def in stuff.h but it doesn't find it.
  for (const std::vector<uint32_t> &c : mf.fragment_packed_sizes) {
    os << "  [";

    for (auto fps : c) {
      os << fps << ", ";
    }
    os << "], " << std::endl;
  }
  os << "  ], ";

  os << std::endl
     << "  version_id = " << mf.version_id << "," << std::endl
     << "  timestamp = " << mf.timestamp // TODO: decent formatting?
     << "} ";
  return os;
}
}
}
