#include "miniverse/geo/raster.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb_bytes.hpp"

namespace miniverse::geo::wkb {

namespace {

constexpr std::uint16_t RASTER_VERSION = 0;

// A band's first byte: flags in the high bits, the pixel type in the low four.
constexpr std::uint8_t OUT_DB_FLAG = 0x80U;
constexpr std::uint8_t HAS_NODATA_FLAG = 0x40U;
constexpr std::uint8_t PIXEL_TYPE_MASK = 0x0FU;

[[nodiscard]] std::uint16_t dimension(std::size_t pixels, const char* what) {
  if ( pixels > std::numeric_limits<std::uint16_t>::max() ) {
    throw std::invalid_argument("wkb: a raster's " + std::string(what) + " of " + std::to_string(pixels) + " is more than WKB can hold (65535)");
  }

  return static_cast<std::uint16_t>(pixels);
}

}  // namespace

template <Pixel pixel_t>
std::vector<std::byte> write(const Raster<pixel_t>& raster) {
  raster.check_pixel_count();

  Writer out;
  out.number(RASTER_VERSION);
  out.number(std::uint16_t{1});  // bands
  out.number(raster.pixel_width);
  out.number(-raster.pixel_height);
  out.number(raster.west);
  out.number(raster.north);
  out.number(0.0);  // skew
  out.number(0.0);
  out.number(WGS84_SRID);
  out.number(dimension(raster.width, "width"));
  out.number(dimension(raster.height, "height"));

  out.byte(std::byte{static_cast<std::uint8_t>(HAS_NODATA_FLAG | PixelType<pixel_t>::CODE)});
  out.number(raster.nodata);
  for ( const pixel_t pixel : raster.pixels ) {
    out.number(pixel);
  }

  return std::move(out).bytes();
}

template <Pixel pixel_t>
Raster<pixel_t> read_raster(std::span<const std::byte> bytes) {
  Reader reader(bytes);
  reader.byte_order();

  if ( const auto version = reader.number<std::uint16_t>(); version != RASTER_VERSION ) {
    malformed("raster WKB version " + std::to_string(version) + ", expected 0");
  }

  if ( const auto bands = reader.number<std::uint16_t>(); bands != 1 ) {
    malformed("a raster with " + std::to_string(bands) + " bands: only a raster of one band is read");
  }

  Raster<pixel_t> raster;
  const auto      scale_x = reader.number<double>();
  const auto      scale_y = reader.number<double>();
  raster.west = reader.number<double>();
  raster.north = reader.number<double>();
  const auto skew_x = reader.number<double>();
  const auto skew_y = reader.number<double>();

  if ( skew_x != 0 || skew_y != 0 ) {
    malformed("the raster is skewed: only a raster that is north up, without skew, is read");
  }

  if ( scale_x <= 0 || scale_y >= 0 ) {
    malformed("the raster's pixels run west or north: only a raster that is north up is read");
  }

  if ( const auto srid = reader.number<std::int32_t>(); srid != WGS84_SRID ) {
    malformed("the raster has SRID " + std::to_string(srid) + ", not " + std::to_string(WGS84_SRID));
  }

  raster.pixel_width = scale_x;
  raster.pixel_height = -scale_y;
  raster.width = reader.number<std::uint16_t>();
  raster.height = reader.number<std::uint16_t>();

  const auto band = std::to_integer<std::uint8_t>(reader.byte());
  if ( (band & OUT_DB_FLAG) != 0 ) {
    malformed("the band is out-db, in a file outside the database: only a band stored in the database is read");
  }

  if ( (band & HAS_NODATA_FLAG) == 0 ) {
    malformed("the band has no nodata value");
  }

  if ( (band & PIXEL_TYPE_MASK) != PixelType<pixel_t>::CODE ) {
    malformed(
        "the band's pixel type is number " + std::to_string(band & PIXEL_TYPE_MASK) + ", not " + std::string(PixelType<pixel_t>::NAME) + " (number " +
        std::to_string(PixelType<pixel_t>::CODE) + ")"
    );
  }

  raster.nodata = reader.number<pixel_t>();

  const std::size_t size = reader.checked_count(raster.width * raster.height, sizeof(pixel_t));
  raster.pixels.reserve(size);
  for ( std::size_t i = 0; i < size; ++i ) {
    raster.pixels.push_back(reader.number<pixel_t>());
  }

  if ( ! reader.at_end() ) {
    malformed(std::to_string(reader.remaining()) + " bytes are left after the raster");
  }

  return raster;
}

template std::vector<std::byte> write<std::int8_t>(const Raster<std::int8_t>& raster);
template std::vector<std::byte> write<std::uint8_t>(const Raster<std::uint8_t>& raster);
template std::vector<std::byte> write<std::int16_t>(const Raster<std::int16_t>& raster);
template std::vector<std::byte> write<std::uint16_t>(const Raster<std::uint16_t>& raster);
template std::vector<std::byte> write<std::int32_t>(const Raster<std::int32_t>& raster);
template std::vector<std::byte> write<std::uint32_t>(const Raster<std::uint32_t>& raster);
template std::vector<std::byte> write<float>(const Raster<float>& raster);
template std::vector<std::byte> write<double>(const Raster<double>& raster);

template Raster<std::int8_t>   read_raster<std::int8_t>(std::span<const std::byte> bytes);
template Raster<std::uint8_t>  read_raster<std::uint8_t>(std::span<const std::byte> bytes);
template Raster<std::int16_t>  read_raster<std::int16_t>(std::span<const std::byte> bytes);
template Raster<std::uint16_t> read_raster<std::uint16_t>(std::span<const std::byte> bytes);
template Raster<std::int32_t>  read_raster<std::int32_t>(std::span<const std::byte> bytes);
template Raster<std::uint32_t> read_raster<std::uint32_t>(std::span<const std::byte> bytes);
template Raster<float>         read_raster<float>(std::span<const std::byte> bytes);
template Raster<double>        read_raster<double>(std::span<const std::byte> bytes);

}  // namespace miniverse::geo::wkb
