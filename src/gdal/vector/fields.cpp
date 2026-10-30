#include "gdal/vector/fields.hpp"

#include <ogr_core.h>
#include <ogr_feature.h>

#include <cpl_json.h>

#include "schemacht/json/json.hpp"

#include <cmath>
#include <cstdint>
#include <ogrsf_frmts.h>
#include <stdexcept>
#include <string>

#include "miniverse/gdal/vector.hpp"

namespace miniverse::gdal::detail {

namespace {

/**
 * @return `value` as a JSON value of its own, to add to an object under any name. GDAL before 3.8
 * makes a value only as a member of an object, under a name it splits at `/`; so it is made under a
 * plain name and taken out again.
 */
template <typename value_t>
[[nodiscard]] CPLJSONObject json_value(value_t value)
{
  CPLJSONObject holder;
  holder.Add("value", value);

  return holder.GetObj("value");
}

}  // namespace

int id_field_of(OGRLayer& layer, const VectorSource& source)
{
  if ( source.id_field.empty() ) {
    return NO_FIELD;
  }

  const OGRFeatureDefn* fields = layer.GetLayerDefn();
  const int             index  = fields->GetFieldIndex(source.id_field.c_str());
  if ( index < 0 ) {
    throw std::runtime_error(
        "'" + source.path + "' has no field '" + source.id_field + "' to take ids from");
  }

  if ( const OGRFieldType type = fields->GetFieldDefn(index)->GetType();
       type != OFTInteger && type != OFTInteger64 )
  {
    throw std::runtime_error("the field '" +
                             source.id_field +
                             "' of '" +
                             source.path +
                             "' is not a whole number, so it can't be the id");
  }

  return index;
}

schemacht::json::Json tags_of(const OGRFeature& feature, int id_field)
{
  const OGRFeatureDefn* fields = feature.GetDefnRef();
  CPLJSONObject         tags;

  for ( int i = 0; i < fields->GetFieldCount(); ++i ) {
    if ( i == id_field || ! feature.IsFieldSetAndNotNull(i) ) {
      continue;
    }

    // Added by its whole name: `Add` would take a `/` in it for a path, and nest or drop the value.
    const OGRFieldDefn* field = fields->GetFieldDefn(i);
    const OGRFieldType  type  = field->GetType();
    if ( type == OFTInteger && field->GetSubType() == OFSTBoolean ) {
      tags.AddNoSplitName(field->GetNameRef(), json_value(feature.GetFieldAsInteger(i) != 0));
    }
    else if ( type == OFTInteger || type == OFTInteger64 ) {
      tags.AddNoSplitName(field->GetNameRef(), json_value(feature.GetFieldAsInteger64(i)));
    }
    else if ( type == OFTReal && std::isfinite(feature.GetFieldAsDouble(i)) ) {
      tags.AddNoSplitName(field->GetNameRef(), json_value(feature.GetFieldAsDouble(i)));
    }
    else {  // text, dates, lists; and a real that is not a number, which JSON has no way to write
      tags.AddNoSplitName(field->GetNameRef(), json_value(feature.GetFieldAsString(i)));
    }
  }

  return schemacht::json::Json(tags.Format(CPLJSONObject::PrettyFormat::Plain));
}

std::int64_t id_of(const OGRFeature& feature, int id_field, const VectorSource& source)
{
  if ( id_field == NO_FIELD ) {
    if ( feature.GetFID() == OGRNullFID ) {
      throw std::runtime_error(
          "a feature of '" +
          source.path +
          "' has no id of its own: name the field to take ids from (id_field)");
    }

    return feature.GetFID();
  }

  if ( ! feature.IsFieldSetAndNotNull(id_field) ) {
    throw std::runtime_error("feature " +
                             std::to_string(feature.GetFID()) +
                             " of '" +
                             source.path +
                             "' has no '" +
                             source.id_field +
                             "', its id");
  }

  return feature.GetFieldAsInteger64(id_field);
}

}  // namespace miniverse::gdal::detail
