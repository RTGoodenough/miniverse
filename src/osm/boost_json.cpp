// Boost.JSON itself, compiled into miniverse::osm, so that it needs no Boost library to link. In a file of its own: a program
// that brings Boost.JSON too (its own copy of this, or libboost_json) then has the linker take one, not two.

#include <boost/json/src.hpp>  // IWYU pragma: keep
