/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014 Pedro Côrte-Real

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "common/StdAfx.h"
#include "decoders/RawDecoder.h"

namespace RawSpeed {

RawDecoder::RawDecoder(FileMap* file) : mRaw(RawImage::create()), mFile(file) {
  decoderVersion = 0;
  failOnUnknown = FALSE;
  interpolateBadPixels = TRUE;
  applyStage1DngOpcodes = TRUE;
  applyCrop = TRUE;
  uncorrectedRawValues = FALSE;
  fujiRotate = TRUE;
}

void RawDecoder::decodeUncompressed(TiffIFD *rawIFD, BitOrder order) {
  uint32 nslices = rawIFD->getEntry(STRIPOFFSETS)->count;
  TiffEntry *offsets = rawIFD->getEntry(STRIPOFFSETS);
  TiffEntry *counts = rawIFD->getEntry(STRIPBYTECOUNTS);
  uint32 yPerSlice = rawIFD->getEntry(ROWSPERSTRIP)->getInt();
  uint32 width = rawIFD->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = rawIFD->getEntry(IMAGELENGTH)->getInt();
  uint32 bitPerPixel = rawIFD->getEntry(BITSPERSAMPLE)->getInt();

  vector<RawSlice> slices;
  uint32 offY = 0;

  for (uint32 s = 0; s < nslices; s++) {
    RawSlice slice;
    slice.offset = offsets->getInt(s);
    slice.count = counts->getInt(s);
    if (offY + yPerSlice > height)
      slice.h = height - offY;
    else
      slice.h = yPerSlice;

    offY += yPerSlice;

    if (mFile->isValid(slice.offset, slice.count)) // Only decode if size is valid
      slices.push_back(slice);
  }

  if (0 == slices.size())
    ThrowRDE("RAW Decoder: No valid slices found. File probably truncated.");

  mRaw->dim = iPoint2D(width, offY);
  mRaw->createData();
  mRaw->whitePoint = (1<<bitPerPixel)-1;

  offY = 0;
  for (uint32 i = 0; i < slices.size(); i++) {
    RawSlice slice = slices[i];
    ByteStream in(mFile, slice.offset, slice.count);
    iPoint2D size(width, slice.h);
    iPoint2D pos(0, offY);
    bitPerPixel = (int)((uint64)((uint64)slice.count * 8u) / (slice.h * width));
    try {
      readUncompressedRaw(in, size, pos, width*bitPerPixel / 8, bitPerPixel, order);
    } catch (RawDecoderException &e) {
      if (i>0)
        mRaw->setError(e.what());
      else
        throw;
    } catch (IOException &e) {
      if (i>0)
        mRaw->setError(e.what());
      else
        ThrowRDE("RAW decoder: IO error occurred in first slice, unable to decode more. Error is: %s", e.what());
    }
    offY += slice.h;
  }
}

void RawDecoder::readUncompressedRaw(ByteStream &input, iPoint2D& size, iPoint2D& offset, int inputPitch, int bitPerPixel, BitOrder order) {
  uchar8* data = mRaw->getData();
  uint32 outPitch = mRaw->pitch;
  uint64 w = size.x;
  uint64 h = size.y;
  uint32 cpp = mRaw->getCpp();
  uint64 ox = offset.x;
  uint64 oy = offset.y;

  if (input.getRemainSize() < (inputPitch*h)) {
    if ((int)input.getRemainSize() > inputPitch) {
      h = input.getRemainSize() / inputPitch - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }
  if (bitPerPixel > 16 && mRaw->getDataType() == TYPE_USHORT16)
    ThrowRDE("readUncompressedRaw: Unsupported bit depth");

  uint32 skipBits = inputPitch - w * cpp * bitPerPixel / 8;  // Skip per line
  if (oy > (uint64) mRaw->dim.y)
    ThrowRDE("readUncompressedRaw: Invalid y offset");
  if (ox + size.x > (uint64)mRaw->dim.x)
    ThrowRDE("readUncompressedRaw: Invalid x offset");

  uint64 y = oy;
  h = MIN(h + oy, (uint32)mRaw->dim.y);

  if (mRaw->getDataType() == TYPE_FLOAT32)
  {
    if (bitPerPixel != 32)
      ThrowRDE("readUncompressedRaw: Only 32 bit float point supported");
    BitBlt(&data[offset.x*sizeof(float)*cpp+y*outPitch], outPitch,
        input.getData(inputPitch*(h-y)), inputPitch, w*mRaw->getBpp(), h - y);
    return;
  }

  if (BitOrder_Jpeg == order) {
    BitPumpMSB bits(input);
    w *= cpp;
    for (; y < h; y++) {
      ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)*cpp+y*outPitch];
      bits.checkPos();
      for (uint32 x = 0 ; x < w; x++) {
        uint32 b = bits.getBits(bitPerPixel);
        dest[x] = b;
      }
      bits.skipBits(skipBits);
    }
  } else if (BitOrder_Jpeg16 == order) {
    BitPumpMSB16 bits(input);
    w *= cpp;
    for (; y < h; y++) {
      ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)*cpp+y*outPitch];
      bits.checkPos();
      for (uint32 x = 0 ; x < w; x++) {
        uint32 b = bits.getBits(bitPerPixel);
        dest[x] = b;
      }
      bits.skipBits(skipBits);
    }
  } else if (BitOrder_Jpeg32 == order) {
    BitPumpMSB32 bits(input);
    w *= cpp;
    for (; y < h; y++) {
      ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)*cpp+y*outPitch];
      bits.checkPos();
      for (uint32 x = 0 ; x < w; x++) {
        uint32 b = bits.getBits(bitPerPixel);
        dest[x] = b;
      }
      bits.skipBits(skipBits);
    }
  } else {
    if (bitPerPixel == 16 && getHostEndianness() == little)  {
      BitBlt(&data[offset.x*sizeof(ushort16)*cpp+y*outPitch], outPitch,
             input.getData(inputPitch*(h-y)), inputPitch, w*mRaw->getBpp(), h - y);
      return;
    }
    if (bitPerPixel == 12 && (int)w == inputPitch * 8 / 12 && getHostEndianness() == little)  {
      Decode12BitRaw(input, w, h);
      return;
    }
    BitPumpPlain bits(input);
    w *= cpp;
    for (; y < h; y++) {
      ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)+y*outPitch];
      bits.checkPos();
      for (uint32 x = 0 ; x < w; x++) {
        uint32 b = bits.getBits(bitPerPixel);
        dest[x] = b;
      }
      bits.skipBits(skipBits);
    }
  }
}

void RawDecoder::Decode8BitRaw(ByteStream &input, uint32 w, uint32 h) {
  if (input.getRemainSize() < w*h) {
    if ((uint32)input.getRemainSize() > w) {
      h = input.getRemainSize() / w - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("Decode8BitRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*h);
  uint32 random = 0;
  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 1) {
      if (uncorrectedRawValues)
        dest[x] = *in++;
      else
        mRaw->setWithLookUp(*in++, (uchar8*)&dest[x], &random);
    }
  }
}

void RawDecoder::Decode12BitRaw(ByteStream &input, uint32 w, uint32 h) {
  if(w<2) ThrowIOE("Are you mad? 1 pixel wide raw images are no fun");

  if (input.getRemainSize() < ((w*12/8)*h)) {
    if ((uint32)input.getRemainSize() > (w*12/8)) {
      h = input.getRemainSize() / (w*12/8) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*12/8*h);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 2) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = g1 | ((g2 & 0xf) << 8);
      uint32 g3 = *in++;
      dest[x+1] = (g2 >> 4) | (g3 << 4);
    }
  }
}

void RawDecoder::Decode12BitRawWithControl(ByteStream &input, uint32 w, uint32 h) {
  if(w<2) ThrowIOE("Are you mad? 1 pixel wide raw images are no fun");

  // Calulate expected bytes per line.
  uint32 perline = (w*12/8);
  // Add skips every 10 pixels
  perline += ((w + 2) / 10);

  // If file is too short, only decode as many lines as we have
  if (input.getRemainSize() <= perline) {
    ThrowIOE("Decode12BitRawBEWithControl: Not enough data to decode a single line. Image file truncated.");
  } else if (input.getRemainSize() < (perline*h)) {
    h = input.getRemainSize() / perline - 1;
    mRaw->setError("Image truncated (file is too short)");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(perline*h);

  uint32 x;
  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (x = 0 ; x < w; x += 2) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = g1 | ((g2 & 0xf) << 8);
      uint32 g3 = *in++;
      dest[x+1] = (g2 >> 4) | (g3 << 4);
      if ((x % 10) == 8)
        in++;
    }
  }
}

void RawDecoder::Decode12BitRawBEWithControl(ByteStream &input, uint32 w, uint32 h) {
  if(w<2) ThrowIOE("Are you mad? 1 pixel wide raw images are no fun");

  // Calulate expected bytes per line.
  uint32 perline = (w*12/8);
  // Add skips every 10 pixels
  perline += ((w + 2) / 10);

  // If file is too short, only decode as many lines as we have
  if (input.getRemainSize() <= perline) {
    ThrowIOE("Decode12BitRawBEWithControl: Not enough data to decode a single line. Image file truncated.");
  } else if (input.getRemainSize() < (perline*h)) {
    h = input.getRemainSize() / perline - 1;
    mRaw->setError("Image truncated (file is too short)");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(perline*h);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0; x < w; x += 2) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = (g1 << 4) | (g2 >> 4);
      uint32 g3 = *in++;
      dest[x+1] = ((g2 & 0x0f) << 8) | g3;
      if ((x % 10) == 8)
        in++;
    }
  }
}

void RawDecoder::Decode12BitRawBE(ByteStream &input, uint32 w, uint32 h) {
  if(w<2) ThrowIOE("Are you mad? 1 pixel wide raw images are no fun");

  if (input.getRemainSize() < ((w*12/8)*h)) {
    if ((uint32)input.getRemainSize() > (w*12/8)) {
      h = input.getRemainSize() / (w*12/8) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*12/8*h);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 2) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = (g1 << 4) | (g2 >> 4);
      uint32 g3 = *in++;
      dest[x+1] = ((g2 & 0x0f) << 8) | g3;
    }
  }
}

void RawDecoder::Decode12BitRawBEInterlaced(ByteStream &input, uint32 w, uint32 h) {
  if(w<2) ThrowIOE("Are you mad? 1 pixel wide raw images are no fun");

  if (input.getRemainSize() < ((w*12/8)*h)) {
    if ((uint32)input.getRemainSize() > (w*12/8)) {
      h = input.getRemainSize() / (w*12/8) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8* in = input.peekData((w*12/8)*h);
  uint32 half = (h+1) >> 1;
  for (uint32 row = 0; row < h; row++) {
    uint32 y = row % half * 2 + row / half;
    ushort16* dest = (ushort16*) & data[y*pitch];
    if (y == 1) {
      // The second field starts at a 2048 byte aligment
      uint32 offset = ((half*w*3/2 >> 11) + 1) << 11;
      if (offset > input.getRemainSize())
        ThrowIOE("Decode12BitSplitRaw: Trying to jump to invalid offset %d", offset);
      in = input.peekData(input.getRemainSize()) + offset;
    }
    for (uint32 x = 0 ; x < w; x += 2) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = (g1 << 4) | (g2 >> 4);
      uint32 g3 = *in++;
      dest[x+1] = ((g2 & 0x0f) << 8) | g3;
    }
  }
  input.skipBytes(input.getRemainSize());
}

void RawDecoder::Decode12BitRawBEunpacked(ByteStream &input, uint32 w, uint32 h) {
  if (input.getRemainSize() < w*h*2) {
    if ((uint32)input.getRemainSize() > w*2) {
      h = input.getRemainSize() / (w*2) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*h*2);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 1) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = ((g1 & 0x0f) << 8) | g2;
    }
  }
}

void RawDecoder::Decode12BitRawBEunpackedLeftAligned(ByteStream &input, uint32 w, uint32 h) {
  if (input.getRemainSize() < w*h*2) {
    if ((uint32)input.getRemainSize() > w*2) {
      h = input.getRemainSize() / (w*2) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*h*2);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 1) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = (((g1 << 8) | (g2 & 0xf0)) >> 4);
    }
  }
}

void RawDecoder::Decode14BitRawBEunpacked(ByteStream &input, uint32 w, uint32 h) {
  if (input.getRemainSize() < w*h*2) {
    if ((uint32)input.getRemainSize() > w*2) {
      h = input.getRemainSize() / (w*2) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*h*2);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 1) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = ((g1 & 0x3f) << 8) | g2;
    }
  }
}

void RawDecoder::Decode16BitRawUnpacked(ByteStream &input, uint32 w, uint32 h) {
  if (input.getRemainSize() < w*h*2) {
    if ((uint32)input.getRemainSize() > w*2) {
      h = input.getRemainSize() / (w*2) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*h*2);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 1) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = (g2 << 8) | g1;
    }
  }
}

void RawDecoder::Decode16BitRawBEunpacked(ByteStream &input, uint32 w, uint32 h) {
  if (input.getRemainSize() < w*h*2) {
    if ((uint32)input.getRemainSize() > w*2) {
      h = input.getRemainSize() / (w*2) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*h*2);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 1) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = (g1 << 8) | g2;
    }
  }
}

void RawDecoder::Decode12BitRawUnpacked(ByteStream &input, uint32 w, uint32 h) {
  if (input.getRemainSize() < w*h*2) {
    if ((uint32)input.getRemainSize() > w*2) {
      h = input.getRemainSize() / (w*2) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData(w*h*2);

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 1) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = ((g2 << 8) | g1) >> 4;
    }
  }
}

bool RawDecoder::checkCameraSupported(CameraMetaData *meta, string make,
                                      string model, const string &mode) {
  TrimSpaces(make);
  TrimSpaces(model);
  mRaw->metadata.make = make;
  mRaw->metadata.model = model;
  Camera* cam = meta->getCamera(make, model, mode);
  if (!cam) {
    writeLog(DEBUG_PRIO_WARNING, "Unable to find camera in database: '%s' '%s' "
                                 "'%s'\nPlease consider providing samples on "
                                 "<https://raw.pixls.us/>, thanks!\n",
             make.c_str(), model.c_str(), mode.c_str());

    if (failOnUnknown)
      ThrowRDE("Camera '%s' '%s', mode '%s' not supported, and not allowed to guess. Sorry.", make.c_str(), model.c_str(), mode.c_str());

    // Assume the camera can be decoded, but return false, so decoders can see that we are unsure.
    return false;
  }

  if (!cam->supported)
    ThrowRDE("Camera not supported (explicit). Sorry.");

  if (cam->decoderVersion > decoderVersion)
    ThrowRDE("Camera not supported in this version. Update RawSpeed for support.");

  hints = cam->hints;
  return true;
}

void RawDecoder::setMetaData(CameraMetaData *meta, string make, string model,
                             const string &mode, int iso_speed) {
  mRaw->metadata.isoSpeed = iso_speed;
  TrimSpaces(make);
  TrimSpaces(model);
  Camera *cam = meta->getCamera(make, model, mode);
  if (!cam) {
    writeLog(DEBUG_PRIO_INFO, "ISO:%d\n", iso_speed);
    writeLog(DEBUG_PRIO_WARNING, "Unable to find camera in database: '%s' '%s' "
                                 "'%s'\nPlease consider providing samples on "
                                 "<https://raw.pixls.us/>, thanks!\n",
             make.c_str(), model.c_str(), mode.c_str());

    if (failOnUnknown)
      ThrowRDE("Camera '%s' '%s', mode '%s' not supported, and not allowed to guess. Sorry.", make.c_str(), model.c_str(), mode.c_str());

    return;
  }

  mRaw->cfa = cam->cfa;
  mRaw->metadata.canonical_make = cam->canonical_make;
  mRaw->metadata.canonical_model = cam->canonical_model;
  mRaw->metadata.canonical_alias = cam->canonical_alias;
  mRaw->metadata.canonical_id = cam->canonical_id;
  mRaw->metadata.make = make;
  mRaw->metadata.model = model;
  mRaw->metadata.mode = mode;

  if (applyCrop) {
    iPoint2D new_size = cam->cropSize;

    // If crop size is negative, use relative cropping
    if (new_size.x <= 0)
      new_size.x = mRaw->dim.x - cam->cropPos.x + new_size.x;

    if (new_size.y <= 0)
      new_size.y = mRaw->dim.y - cam->cropPos.y + new_size.y;

    mRaw->subFrame(iRectangle2D(cam->cropPos, new_size));

    // Shift CFA to match crop
    if (cam->cropPos.x & 1)
      mRaw->cfa.shiftLeft();
    if (cam->cropPos.y & 1)
      mRaw->cfa.shiftDown();
  }

  const CameraSensorInfo *sensor = cam->getSensorInfo(iso_speed);
  mRaw->blackLevel = sensor->mBlackLevel;
  mRaw->whitePoint = sensor->mWhiteLevel;
  mRaw->blackAreas = cam->blackAreas;
  if (mRaw->blackAreas.empty() && !sensor->mBlackLevelSeparate.empty()) {
    if (mRaw->isCFA && mRaw->cfa.size.area() <= sensor->mBlackLevelSeparate.size()) {
      for (uint32 i = 0; i < mRaw->cfa.size.area(); i++) {
        mRaw->blackLevelSeparate[i] = sensor->mBlackLevelSeparate[i];
      }
    } else if (!mRaw->isCFA && mRaw->getCpp() <= sensor->mBlackLevelSeparate.size()) {
      for (uint32 i = 0; i < mRaw->getCpp(); i++) {
        mRaw->blackLevelSeparate[i] = sensor->mBlackLevelSeparate[i];
      }
    }
  }

  // Allow overriding individual blacklevels. Values are in CFA order
  // (the same order as the in the CFA tag)
  // A hint could be:
  // <Hint name="override_cfa_black" value="10,20,30,20"/>
  if (cam->hints.find(string("override_cfa_black")) != cam->hints.end()) {
    string rgb = cam->hints.find(string("override_cfa_black"))->second;
    vector<string> v = split_string(rgb, ',');
    if (v.size() != 4) {
      mRaw->setError("Expected 4 values '10,20,30,20' as values for override_cfa_black hint.");
    } else {
      for (int i = 0; i < 4; i++) {
        mRaw->blackLevelSeparate[i] = atoi(v[i].c_str());
      }
    }
  }
}


void *RawDecoderDecodeThread(void *_this) {
  RawDecoderThread* me = (RawDecoderThread*)_this;
  try {
     me->parent->decodeThreaded(me);
  } catch (RawDecoderException &ex) {
    me->parent->mRaw->setError(ex.what());
  } catch (IOException &ex) {
    me->parent->mRaw->setError(ex.what());
  }
  return NULL;
}

void RawDecoder::startThreads() {
#ifdef NO_PTHREAD
  uint32 threads = 1;
  RawDecoderThread t;
  t.start_y = 0;
  t.end_y = mRaw->dim.y;
  t.parent = this;
  RawDecoderDecodeThread(&t);
#else
  uint32 threads;
  bool fail = false;
  threads = MIN(mRaw->dim.y, getThreadCount());
  int y_offset = 0;
  int y_per_thread = (mRaw->dim.y + threads - 1) / threads;
  RawDecoderThread *t = new RawDecoderThread[threads];

  /* Initialize and set thread detached attribute */
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  for (uint32 i = 0; i < threads; i++) {
    t[i].start_y = y_offset;
    t[i].end_y = MIN(y_offset + y_per_thread, mRaw->dim.y);
    t[i].parent = this;
    if (pthread_create(&t[i].threadid, &attr, RawDecoderDecodeThread, &t[i]) != 0) {
      // If a failure occurs, we need to wait for the already created threads to finish
      threads = i-1;
      fail = true;
    }
    y_offset = t[i].end_y;
  }

  for (uint32 i = 0; i < threads; i++) {
    pthread_join(t[i].threadid, NULL);
  }
  pthread_attr_destroy(&attr);
  delete[] t;

  if (fail) {
    ThrowRDE("RawDecoder::startThreads: Unable to start threads");
  }
#endif

  if (mRaw->errors.size() >= threads)
    ThrowRDE("RawDecoder::startThreads: All threads reported errors. Cannot load image.");
}

void RawDecoder::decodeThreaded(RawDecoderThread * t) {
  ThrowRDE("Internal Error: This class does not support threaded decoding");
}

RawSpeed::RawImage RawDecoder::decodeRaw()
{
  try {
    RawImage raw = decodeRawInternal();
    if(hints.find("pixel_aspect_ratio") != hints.end()) {
      stringstream convert(hints.find("pixel_aspect_ratio")->second);
      convert >> raw->metadata.pixelAspectRatio;
    }
    if (interpolateBadPixels)
      raw->fixBadPixels();
    return raw;
  } catch (TiffParserException &e) {
    ThrowRDE("%s", e.what());
  } catch (FileIOException &e) {
    ThrowRDE("%s", e.what());
  } catch (IOException &e) {
    ThrowRDE("%s", e.what());
  }
  return NULL;
}

void RawDecoder::decodeMetaData(CameraMetaData *meta)
{
  try {
    return decodeMetaDataInternal(meta);
  } catch (TiffParserException &e) {
    ThrowRDE("%s", e.what());
  } catch (FileIOException &e) {
    ThrowRDE("%s", e.what());
  } catch (IOException &e) {
    ThrowRDE("%s", e.what());
  }
}

void RawDecoder::checkSupport(CameraMetaData *meta)
{
  try {
    return checkSupportInternal(meta);
  } catch (TiffParserException &e) {
    ThrowRDE("%s", e.what());
  } catch (FileIOException &e) {
    ThrowRDE("%s", e.what());
  } catch (IOException &e) {
    ThrowRDE("%s", e.what());
  }
}

void RawDecoder::startTasks( uint32 tasks )
{
  uint32 threads;
  threads = min(tasks, getThreadCount());
  int ctask = 0;
  RawDecoderThread *t = new RawDecoderThread[threads];

  // We don't need a thread
  if (threads == 1) {
    t[0].parent = this;
    while ((uint32)ctask < tasks) {
      t[0].taskNo = ctask++;
      try {
        decodeThreaded(&t[0]);
      } catch (RawDecoderException &ex) {
        mRaw->setError(ex.what());
      } catch (IOException &ex) {
        mRaw->setError(ex.what());
      }
    }
    delete[] t;
    return;
  }

#ifndef NO_PTHREAD
  pthread_attr_t attr;

  /* Initialize and set thread detached attribute */
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  /* TODO: Create a way to re-use threads */
  void *status;
  while ((uint32)ctask < tasks) {
    for (uint32 i = 0; i < threads && (uint32)ctask < tasks; i++) {
      t[i].taskNo = ctask++;
      t[i].parent = this;
      pthread_create(&t[i].threadid, &attr, RawDecoderDecodeThread, &t[i]);
    }
    for (uint32 i = 0; i < threads; i++) {
      pthread_join(t[i].threadid, &status);
    }
  }

  if (mRaw->errors.size() >= tasks)
    ThrowRDE("RawDecoder::startThreads: All threads reported errors. Cannot load image.");

  delete[] t;
#else
  ThrowRDE("Unreachable");
#endif
}

} // namespace RawSpeed