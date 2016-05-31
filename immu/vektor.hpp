
#pragma once

#include <cstdlib>
#include <memory>
#include <eggs/variant.hpp>
#include <boost/iterator/iterator_facade.hpp>

namespace immu {

constexpr auto branching_log  = 5;
constexpr auto branching      = 1 << branching_log;
constexpr auto branching_mask = branching - 1;

namespace detail {

template <typename T>
struct vektor_node;
template <typename T>
using vektor_node_ptr = std::shared_ptr<vektor_node<T> >;

template <typename T>
using vektor_leaf  = std::array<T, branching>;
template <typename T>
using vektor_inner = std::array<vektor_node_ptr<T>, branching>;

using eggs::variants::get;

template <typename T>
struct vektor_node : eggs::variant<vektor_leaf<T>,
                                   vektor_inner<T>>
{
    using base_t = eggs::variant<vektor_leaf<T>,
                                 vektor_inner<T>>;
    using base_t::base_t;

    vektor_inner<T>& inner() & { return get<vektor_inner<T>>(*this); }
    const vektor_inner<T>& inner() const& { return get<vektor_inner<T>>(*this); }
    vektor_inner<T>&& inner() && { return get<vektor_inner<T>>(std::move(*this)); }

    vektor_leaf<T>& leaf() & { return get<vektor_leaf<T>>(*this); }
    const vektor_leaf<T>& leaf() const& { return get<vektor_leaf<T>>(*this); }
    vektor_leaf<T>&& leaf() && { return get<vektor_leaf<T>>(std::move(*this)); }
};

} // namespace detail

template <typename T>
class vektor
{
    using inner_t  = detail::vektor_inner<T>;
    using leaf_t   = detail::vektor_leaf<T>;
    using node_t   = detail::vektor_node<T>;
    using node_ptr = detail::vektor_node_ptr<T>;

    static const node_ptr empty_leaf_;
    static const node_ptr empty_inner_;
    static const vektor   empty_;

    vektor(std::size_t size, unsigned shift, node_ptr root, node_ptr tail)
        : size_(size)
        , shift_(shift)
        , root_(std::move(root))
        , tail_(std::move(tail))
    {}

    auto tail_offset_() const
    {
        return size_ < 32 ? 0 : ((size_ - 1) >> branching_log) << branching_log;
    }

    const leaf_t& array_for_(std::size_t index) const
    {
        assert(index < size_);

        if (index >= tail_offset_())
            return tail_->leaf();

        auto node = root_.get();
        for (auto level = shift_; level; level -= branching_log) {
            node = node->inner() [(index >> level) & branching_mask].get();
        }
        return node->leaf();
    }

    node_ptr make_path_(unsigned level, node_ptr node) const
    {
        return level == 0
            ? node
            : make_node_(inner_t{{ make_path_(level - branching_log,
                                              std::move(node)) }});
    }

    node_ptr push_tail_(unsigned level,
                        const node_t& parent_,
                        node_ptr tail) const
    {
        const auto& parent   = parent_.inner();
        auto new_parent_node = make_node_(parent);
        auto& new_parent     = new_parent_node->inner();
        auto idx             = ((size_ - 1) >> level) & branching_mask;
        auto next_node =
            level == branching_log ? std::move(tail) :
            parent[idx]            ? push_tail_(level - branching_log,
                                                *parent[idx],
                                                std::move(tail)) :
            /* otherwise */          make_path_(level - branching_log,
                                                std::move(tail));
        new_parent[idx] = next_node;
        return new_parent_node;
    }

    template <typename ...Ts>
    static auto make_node_(Ts&& ...xs)
    {
        return std::make_shared<node_t>(std::forward<Ts>(xs)...);
    }

public:
    using value_type = T;
    using reference_type = const T&;

    struct iterator :
        boost::iterator_facade<iterator,
                               value_type,
                               boost::random_access_traversal_tag,
                               reference_type>
    {
        iterator() = default;

    private:
        friend class vektor;
        friend class boost::iterator_core_access;

        struct end_t {};

        using leaf_iterator = typename leaf_t::const_iterator;
        const vektor* v_;
        std::size_t   i_;
        std::size_t   base_;
        leaf_iterator curr_;

        iterator(const vektor* v)
            : v_    { v }
            , i_    { 0 }
            , base_ { 0 }
            , curr_ { v->array_for_(0).begin() }
        {
        }

        iterator(const vektor* v, end_t)
            : v_    { v }
            , i_    { v->size_ }
            , base_ { i_ - (i_ & branching_mask) }
            , curr_ { v->array_for_(i_ - 1).begin() + (i_ - base_) }
        {}

        void increment()
        {
            assert(i_ < v_->size_);
            ++i_;
            if (i_ - base_ < branching) {
                ++curr_;
            } else {
                base_ += branching;
                curr_ = v_->array_for_(i_).begin();
            }
        }

        void decrement()
        {
            assert(i_ > 0);
            --i_;
            if (i_ >= base_) {
                --curr_;
            } else {
                base_ -= branching;
                curr_ = std::prev(v_->array_for_(i_).end());
            }
        }

        void advance(std::ptrdiff_t n)
        {
            assert(n <= 0 || i_ + static_cast<std::size_t>(n) <= v_->size_);
            assert(n >= 0 || static_cast<std::size_t>(-n) <= i_);

            i_ += n;
            if (i_ <= base_ && i_ - base_ < branching) {
                curr_ += n;
            } else {
                base_ = i_ - (i_ & branching_mask);
                curr_ = v_->array_for_(i_).begin() + (i_ - base_);
            }
        }

        bool equal(const iterator& other) const
        {
            return i_ == other.i_;
        }

        std::ptrdiff_t distance_to(const iterator& other) const
        {
            return other.i_ > i_
                ?   static_cast<ptrdiff_t>(other.i_ - i_)
                : - static_cast<ptrdiff_t>(i_ - other.i_);
        }

        const T& dereference() const
        {
            return *curr_;
        }
    };

    using const_iterator   = iterator;
    using reverse_iterator = std::reverse_iterator<iterator>;

    vektor() : vektor{empty_} {}
    vektor(const vektor&) = default;
    vektor(vektor&&) = default;
    vektor& operator=(const vektor&) = default;
    vektor& operator=(vektor&&) = default;

    iterator begin() const { return {this}; }
    iterator end()   const { return {this, typename iterator::end_t{}}; }

    reverse_iterator rbegin() const { return reverse_iterator{end()}; }
    reverse_iterator rend()   const { return reverse_iterator{begin()}; }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    const T& operator[] (std::size_t index) const
    {
        const auto& arr = array_for_(index);
        return arr [index & branching_mask];
    }

    vektor push_back(T value) const
    {
        auto tail_size = size_ - tail_offset_();
        if (tail_size < branching) {
            const auto& old_tail = tail_->leaf();
            auto  new_tail_node  = make_node_(leaf_t{});
            auto& new_tail       = new_tail_node->leaf();
            std::copy(old_tail.begin(),
                      old_tail.begin() + tail_size,
                      new_tail.begin());
            new_tail[tail_size] = std::move(value);
            return vektor{ size_ + 1,
                    shift_,
                    root_,
                    std::move(new_tail_node) };
        } else {
            auto new_tail_node = make_node_(leaf_t {{ std::move(value) }});
            return ((size_ >> branching_log) > (1u << shift_))
                ? vektor{ size_ + 1,
                    shift_ + branching_log,
                    make_node_(inner_t{{ root_, make_path_(shift_, tail_) }}),
                    new_tail_node }
                : vektor{ size_ + 1,
                          shift_,
                          push_tail_(shift_, *root_, tail_),
                          new_tail_node };
        }
    }

private:

    std::size_t size_;
    unsigned    shift_;
    node_ptr    root_;
    node_ptr    tail_;
};

template <typename T>
const detail::vektor_node_ptr<T>
vektor<T>::empty_inner_ = vektor<T>::make_node_(detail::vektor_inner<T>{});

template <typename T>
const detail::vektor_node_ptr<T>
vektor<T>::empty_leaf_ = vektor<T>::make_node_(detail::vektor_leaf<T>{});

template <typename T>
const vektor<T>
vektor<T>::empty_ = {0, branching_log, empty_inner_, empty_leaf_};

} // namespace immu
