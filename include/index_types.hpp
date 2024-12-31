#pragma once

#include "include/bit_vector.hpp"
#include "include/integer_codes.hpp"
#include "index.hpp"

#include "builders/builder.hpp"
#include "color_sets/hybrid.hpp"

namespace fulgor {
typedef index<hybrid> hybrid_colors_index_type;
typedef hybrid_colors_index_type index_type;  // in use
}  // namespace fulgor

#include "builders/meta_builder.hpp"
#include "color_sets/meta.hpp"

namespace fulgor {
typedef index<meta<hybrid>> meta_hybrid_colors_index_type;
typedef meta_hybrid_colors_index_type meta_index_type;  // in use
}  // namespace fulgor

#include "builders/differential_builder.hpp"
#include "color_sets/differential.hpp"

namespace fulgor {
typedef index<differential> differential_colors_index_type;
typedef differential_colors_index_type differential_index_type;  // in use
}  // namespace fulgor

#include "color_sets/meta_differential.hpp"
#include "builders/meta_differential_builder.hpp"

namespace fulgor {
typedef index<meta_differential> meta_differential_colors_index_type;
typedef meta_differential_colors_index_type meta_differential_index_type;  // in use
}  // namespace fulgor

#include "color_sets/sliced.hpp"
#include "builders/sliced_builder.hpp"

namespace fulgor{
typedef index<sliced> sliced_colors_index_type;
typedef sliced_colors_index_type sliced_index_type;
}  // namespace fulgor

#include "color_sets/repair.hpp"
#include "builders/repair_builder.hpp"

namespace fulgor{
typedef index<repair> repair_colors_index_type;
typedef repair_colors_index_type repair_index_type;
}  // namespace fulgor
