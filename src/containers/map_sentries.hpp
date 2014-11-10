// Copyright 2010-2012 RethinkDB, all rights reserved.
#ifndef CONTAINERS_MAP_SENTRIES_HPP_
#define CONTAINERS_MAP_SENTRIES_HPP_

#include <map>
#include <utility>

/* `map_insertion_sentry_t` inserts a value into a map on construction, and
removes it in the destructor. */
template<class key_t, class value_t>
class map_insertion_sentry_t {
public:
    map_insertion_sentry_t() : map(nullptr) { }
    map_insertion_sentry_t(std::map<key_t, value_t> *m,
                           const key_t &key,
                           const value_t &value)
        : map(nullptr) {
        reset(m, key, value);
    }

    // For `std::vector.emplace_back` this type needs to be movable, watch
    // https://www.youtube.com/watch?v=ECoLo17nG5c for more information.
    map_insertion_sentry_t(map_insertion_sentry_t && rhs)
        : map(rhs.map),
          it(rhs.it) {
        rhs.map = nullptr;
    }
    map_insertion_sentry_t& operator=(map_insertion_sentry_t && rhs) {
        if (this != &rhs) {
            map = rhs.map;
            it = rhs.it;

            rhs.map = nullptr;
        }
        return *this;
    }

    ~map_insertion_sentry_t() {
        reset();
    }

    void reset() {
        if (map != nullptr) {
            map->erase(it);
            map = nullptr;
        }
    }

    void reset(std::map<key_t, value_t> *m, const key_t &key, const value_t &value) {
        reset();
        map = m;
        std::pair<typename std::map<key_t, value_t>::iterator, bool> iterator_and_is_new =
            map->insert(std::make_pair(key, value));
        rassert(iterator_and_is_new.second, "value to be inserted already "
            "exists. don't do that.");
        it = iterator_and_is_new.first;
    }

private:
    std::map<key_t, value_t> *map;
    typename std::map<key_t, value_t>::iterator it;
};

/* `multimap_insertion_sentry_t` inserts a value into a multimap on
construction, and removes it in the destructor. */
template<class key_t, class value_t>
class multimap_insertion_sentry_t {
public:
    multimap_insertion_sentry_t() : map(nullptr) { }
    multimap_insertion_sentry_t(std::multimap<key_t, value_t> *m,
                                const key_t &key,
                                const value_t &value)
        : map(nullptr) {
        reset(m, key, value);
    }

    multimap_insertion_sentry_t(const multimap_insertion_sentry_t &) = delete;
    multimap_insertion_sentry_t& operator=(const multimap_insertion_sentry_t &) = delete;

    // See above.
    multimap_insertion_sentry_t(multimap_insertion_sentry_t && rhs)
        : map(rhs.map),
          it(rhs.it) {
        rhs.map = nullptr;
    }
    multimap_insertion_sentry_t& operator=(multimap_insertion_sentry_t && rhs) {
        if (this != &rhs) {
            map = rhs.map;
            it = rhs.it;

            rhs.map = nullptr;
        }
        return *this;
    }

    ~multimap_insertion_sentry_t() {
        reset();
    }

    void reset() {
        if (map != nullptr) {
            map->erase(it);
            map = nullptr;
        }
    }

    void reset(std::multimap<key_t, value_t> *m, const key_t &key, const value_t &value) {
        reset();
        map = m;
        it = map->insert(std::make_pair(key, value));
    }

private:
    std::multimap<key_t, value_t> *map;
    typename std::multimap<key_t, value_t>::iterator it;
};

#endif  // CONTAINERS_MAP_SENTRIES_HPP_
