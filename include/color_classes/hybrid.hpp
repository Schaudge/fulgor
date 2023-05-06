#pragma once

#include "inverted_index.hpp"

namespace fulgor::color_classes {

struct hybrid {
    static std::string type() { return "hybrid"; }

    enum list_type { delta_gaps = 0, bitmap = 1, complementary_delta_gaps = 2 };

    void build(build_configuration const& build_config) {
        std::string color_classes_filename = build_config.file_base_name + ".unique_color_classes";
        mm::file_source<uint64_t> mm_index_file(color_classes_filename, mm::advice::sequential);
        inverted_index::iterator it(mm_index_file.data(), mm_index_file.size());
        uint64_t num_docs = it.num_docs();
        uint64_t num_ints = it.num_ints();
        std::cout << "num_docs: " << num_docs << std::endl;
        std::cout << "num_ints: " << num_ints << std::endl;

        m_num_docs = num_docs;

        /* if list contains < sparse_set_threshold_size ints, code it with gaps+delta */
        m_sparse_set_threshold_size = 0.25 * m_num_docs;

        /* if list contains > very_dense_set_threshold_size ints, code it as a complementary set
           with gaps+delta */
        m_very_dense_set_threshold_size = 0.75 * m_num_docs;
        /* otherwise: code it as a bitmap of m_num_docs bits */

        bit_vector_builder bvb;
        bvb.reserve(8 * essentials::GB);

        std::vector<uint64_t> offsets;
        offsets.push_back(0);

        uint64_t num_lists = 0;
        uint64_t num_total_integers = 0;

        std::cout << "m_sparse_set_threshold_size " << m_sparse_set_threshold_size << std::endl;
        std::cout << "m_very_dense_set_threshold_size " << m_very_dense_set_threshold_size
                  << std::endl;

        while (it.has_next()) {
            auto list = it.list();
            uint64_t list_size = list.size();
            /* encode list_size */
            util::write_delta(bvb, list_size);

            if (list_size < m_sparse_set_threshold_size) {
                auto list_it = list.begin();
                uint32_t prev_val = *list_it;
                util::write_delta(bvb, prev_val);
                ++list_it;
                for (uint64_t i = 1; i != list_size; ++i, ++list_it) {
                    uint32_t val = *list_it;
                    assert(val >= prev_val + 1);
                    util::write_delta(bvb, val - (prev_val + 1));
                    prev_val = val;
                }
            } else if (list_size < m_very_dense_set_threshold_size) {
                bit_vector_builder bvb_ints;
                bvb_ints.resize(m_num_docs);
                for (auto x : list) bvb_ints.set(x);
                bvb.append(bvb_ints);
            } else {
                bool first = true;
                uint32_t val = 0;
                uint32_t prev_val = -1;
                uint32_t written = 0;
                for (auto x : list) {
                    while (val < x) {
                        if (first) {
                            util::write_delta(bvb, val);
                            first = false;
                            ++written;
                        } else {
                            assert(val >= prev_val + 1);
                            util::write_delta(bvb, val - (prev_val + 1));
                            ++written;
                        }
                        prev_val = val;
                        ++val;
                    }
                    assert(val == x);
                    val++;  // skip x
                }
                while (val < num_docs) {
                    assert(val >= prev_val + 1);
                    util::write_delta(bvb, val - (prev_val + 1));
                    prev_val = val;
                    ++val;
                    ++written;
                }
                assert(val == num_docs);
                /* complementary_list_size = num_docs - list_size */
                assert(num_docs - list_size <= m_num_docs);
                assert(written == num_docs - list_size);
            }

            offsets.push_back(bvb.num_bits());
            num_total_integers += list_size;
            it.advance_to_next_list();
            num_lists += 1;
            if (num_lists % 500000 == 0) {
                std::cout << "processed " << num_lists << " lists" << std::endl;
            }
        }
        mm_index_file.close();

        std::cout << "processed " << num_lists << " lists" << std::endl;
        std::cout << "num_total_integers " << num_total_integers << std::endl;
        assert(num_total_integers == num_ints);
        assert(num_lists == offsets.size() - 1);

        m_offsets.encode(offsets.begin(), offsets.size(), offsets.back());
        m_colors.swap(bvb.bits());

        std::cout << "  total bits for ints = " << m_colors.size() * 64 << std::endl;
        std::cout << "  total bits per offsets = " << m_offsets.num_bits() << std::endl;
        std::cout << "  total bits = " << m_offsets.num_bits() + m_colors.size() * 64 << std::endl;
        std::cout << "  offsets: " << static_cast<double>(m_offsets.num_bits()) / num_total_integers
                  << " bits/int" << std::endl;
        std::cout << "  lists: " << static_cast<double>(m_colors.size() * 64) / num_total_integers
                  << " bits/int" << std::endl;
    }

    struct forward_iterator {
        forward_iterator(hybrid const* ptr, uint64_t begin)
            : m_ptr(ptr)
            , m_begin(begin)
            , m_num_docs(ptr->m_num_docs)
            , m_pos_in_list(0)
            , m_pos_in_comp_list(0)
            , m_comp_list_size(0)
            , m_comp_val(-1)
            , m_prev_val(-1)
            , m_curr_val(0) {
            m_it = bit_vector_iterator((ptr->m_colors).data(), (ptr->m_colors).size(), begin);
            m_size = util::read_delta(m_it);
            /* set m_type and read the first value */
            if (m_size < ptr->m_sparse_set_threshold_size) {
                m_type = list_type::delta_gaps;
                m_curr_val = util::read_delta(m_it);
            } else if (m_size < ptr->m_very_dense_set_threshold_size) {
                m_type = list_type::bitmap;
                m_begin = m_it.position();  // after m_size
                m_it.at_and_clear_low_bits(m_begin);
                uint64_t pos = m_it.next();
                assert(pos >= m_begin);
                m_curr_val = pos - m_begin;
            } else {
                m_type = list_type::complementary_delta_gaps;
                m_comp_list_size = m_num_docs - m_size;
                if (m_comp_list_size > 0) m_comp_val = util::read_delta(m_it);
                next_comp_val();
            }
        }

        /* this is needed to annul the next_comp_val() done in the constructor
           if we want to iterate through the complemented set */
        void reinit_for_complemented_set_iteration() {
            assert(m_type == list_type::complementary_delta_gaps);
            m_pos_in_comp_list = 0;
            m_prev_val = -1;
            m_curr_val = 0;
            m_it = bit_vector_iterator((m_ptr->m_colors).data(), (m_ptr->m_colors).size(), m_begin);
            util::read_delta(m_it);  // skip m_size
            if (m_comp_list_size > 0) {
                m_comp_val = util::read_delta(m_it);
            } else {
                m_comp_val = m_num_docs;
            }
        }

        uint64_t value() const { return m_curr_val; }
        uint64_t comp_value() const { return m_comp_val; }
        uint64_t operator*() const { return value(); }

        void next() {
            if (m_type == list_type::complementary_delta_gaps) {
                ++m_curr_val;
                if (m_curr_val >= m_num_docs) {  // saturate
                    m_curr_val = m_num_docs;
                    return;
                }
                next_comp_val();
            } else if (m_type == list_type::delta_gaps) {
                m_pos_in_list += 1;
                if (m_pos_in_list >= m_size) {  // saturate
                    m_curr_val = m_num_docs;
                    return;
                }
                m_prev_val = m_curr_val;
                m_curr_val = util::read_delta(m_it) + (m_prev_val + 1);
            } else {
                assert(m_type == list_type::bitmap);
                m_pos_in_list += 1;
                if (m_pos_in_list >= m_size) {  // saturate
                    m_curr_val = m_num_docs;
                    return;
                }
                uint64_t pos = m_it.next();
                assert(pos >= m_begin);
                m_curr_val = pos - m_begin;
            }
        }

        void next_comp() {
            ++m_pos_in_comp_list;
            if (m_pos_in_comp_list >= m_comp_list_size) {  // saturate
                m_comp_val = m_num_docs;
                return;
            }
            m_prev_val = m_comp_val;
            m_comp_val = util::read_delta(m_it) + (m_prev_val + 1);
        }

        void operator++() { next(); }

        /* update the state of the iterator to the element
           which is greater-than or equal-to lower_bound */
        void next_geq(uint64_t lower_bound) {
            assert(lower_bound <= m_num_docs);
            if (m_type == list_type::complementary_delta_gaps) {
                next_geq_comp_val(lower_bound);
                m_curr_val = lower_bound;
            } else {
                while (value() < lower_bound) next();
            }
            assert(value() >= lower_bound);
        }

        uint32_t size() const { return m_size; }
        uint32_t num_docs() const { return m_num_docs; }
        int type() const { return m_type; }

    private:
        hybrid const* m_ptr;
        uint64_t m_begin;
        uint32_t m_num_docs;
        int m_type;

        bit_vector_iterator m_it;
        uint32_t m_pos_in_list;
        uint32_t m_size;

        uint32_t m_pos_in_comp_list;
        uint32_t m_comp_list_size;

        uint32_t m_comp_val;
        uint32_t m_prev_val;
        uint32_t m_curr_val;

        void next_comp_val() {
            while (m_curr_val == m_comp_val) {
                ++m_curr_val;
                ++m_pos_in_comp_list;
                if (m_pos_in_comp_list >= m_comp_list_size) break;
                m_prev_val = m_comp_val;
                m_comp_val = util::read_delta(m_it) + (m_prev_val + 1);
            }
        }

        void next_geq_comp_val(uint64_t lower_bound) {
            while (m_comp_val < lower_bound) {
                ++m_pos_in_comp_list;
                if (m_pos_in_comp_list >= m_comp_list_size) break;
                m_prev_val = m_comp_val;
                m_comp_val = util::read_delta(m_it) + (m_prev_val + 1);
            }
        }
    };

    typedef forward_iterator iterator_type;

    forward_iterator colors(uint64_t color_class_id) const {
        assert(color_class_id < num_color_classes());
        uint64_t begin = m_offsets.access(color_class_id);
        return forward_iterator(this, begin);
    }

    uint32_t num_docs() const { return m_num_docs; }
    uint64_t num_color_classes() const { return m_offsets.size() - 1; }

    uint64_t num_bits() const {
        return (sizeof(m_num_docs) + sizeof(m_sparse_set_threshold_size) +
                sizeof(m_very_dense_set_threshold_size)) *
                   8 +
               m_offsets.num_bits() + essentials::vec_bytes(m_colors) * 8;
    }

    void print_stats() const {
        uint64_t num_buckets = 10;
        assert(num_buckets > 0);
        uint64_t bucket_size = m_num_docs / num_buckets;
        std::vector<uint64_t> num_bits_per_bucket;
        std::vector<uint64_t> num_lists_per_bucket;
        std::vector<uint64_t> num_ints_per_bucket;
        num_bits_per_bucket.resize(num_buckets, 0);
        num_lists_per_bucket.resize(num_buckets, 0);
        num_ints_per_bucket.resize(num_buckets, 0);

        /* Note: we assume lists are sorted by non-decreasing size. */
        const uint64_t num_lists = num_color_classes();
        uint64_t num_total_integers = 0;
        uint64_t curr_list_size_upper_bound = bucket_size;
        for (uint64_t color_class_id = 0, i = 0; color_class_id != m_offsets.size() - 1;
             ++color_class_id) {
            uint64_t offset = m_offsets.access(color_class_id);
            bit_vector_iterator it(m_colors.data(), m_colors.size(), offset);
            uint32_t list_size = util::read_delta(it);
            uint64_t num_bits = m_offsets.access(color_class_id + 1) - offset;
            if (list_size > curr_list_size_upper_bound) {
                i += 1;
                if (i == num_buckets - 1) {
                    curr_list_size_upper_bound = m_num_docs;
                } else {
                    curr_list_size_upper_bound += bucket_size;
                }
            }
            num_bits_per_bucket[i] += num_bits;
            num_lists_per_bucket[i] += 1;
            num_ints_per_bucket[i] += list_size;
            num_total_integers += list_size;
        }

        std::cout << "CCs SPACE BREAKDOWN:\n";
        uint64_t integers = 0;
        uint64_t lists = 0;
        uint64_t bits = 0;
        curr_list_size_upper_bound = 0;
        const uint64_t total_bits = num_bits();
        for (uint64_t i = 0; i != num_buckets; ++i) {
            if (i == num_buckets - 1) {
                curr_list_size_upper_bound = m_num_docs;
            } else {
                curr_list_size_upper_bound += bucket_size;
            }
            if (num_lists_per_bucket[i] > 0) {
                uint64_t n = num_ints_per_bucket[i];
                integers += n;
                lists += num_lists_per_bucket[i];
                bits += num_bits_per_bucket[i];
                std::cout << "num. lists of size > " << (curr_list_size_upper_bound - bucket_size)
                          << " and <= " << curr_list_size_upper_bound << ": "
                          << num_lists_per_bucket[i] << " ("
                          << (num_lists_per_bucket[i] * 100.0) / num_lists
                          << "%) -- integers: " << n << " (" << (n * 100.0) / num_total_integers
                          << "%) -- bits/int: " << static_cast<double>(num_bits_per_bucket[i]) / n
                          << " -- "
                          << static_cast<double>(num_bits_per_bucket[i]) / total_bits * 100.0
                          << "\% of total space" << '\n';
            }
        }
        assert(integers == num_total_integers);
        assert(lists == num_lists);
        std::cout << "  colors: " << static_cast<double>(bits) / integers << " bits/int"
                  << std::endl;
        std::cout << "  offsets: "
                  << static_cast<double>((sizeof(m_num_docs) + sizeof(m_sparse_set_threshold_size) +
                                          sizeof(m_very_dense_set_threshold_size)) *
                                             8 +
                                         m_offsets.num_bits()) /
                         integers
                  << " bits/int" << std::endl;
    }

    template <typename Visitor>
    void visit(Visitor& visitor) {
        visitor.visit(m_num_docs);
        visitor.visit(m_sparse_set_threshold_size);
        visitor.visit(m_very_dense_set_threshold_size);
        visitor.visit(m_offsets);
        visitor.visit(m_colors);
    }

private:
    uint32_t m_num_docs;
    uint32_t m_sparse_set_threshold_size;
    uint32_t m_very_dense_set_threshold_size;
    sshash::ef_sequence<false> m_offsets;
    std::vector<uint64_t> m_colors;
};

}  // namespace fulgor::color_classes