// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include <string>
#include <utility>
#include <vector>

#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/math_utils.hpp"

namespace ql {

template<class T>
class map_acc_term_t : public grouped_seq_op_term_t {
protected:
    map_acc_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(1, 2), optargspec_t({"index"})) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args,
                                       eval_flags_t) const {
        scoped_ptr_t<val_t> v = args->arg(env, 0);
        scoped_ptr_t<val_t> idx = args->optarg(env, "index");
        counted_t<const func_t> func;
        if (args->num_args() == 2) {
            func = args->arg(env, 1)->as_func(GET_FIELD_SHORTCUT);
        }
        if (!func.has() && !idx.has()) {
            // TODO: make this use a table slice.
            if (uses_idx() && v->get_type().is_convertible(val_t::type_t::TABLE)) {
                return on_idx(env->env, v->as_table(), std::move(idx));
            } else {
                return v->as_seq(env->env)->run_terminal(env->env, T(backtrace()));
            }
        } else if (func.has() && !idx.has()) {
            return v->as_seq(env->env)->run_terminal(env->env, T(backtrace(), func));
        } else if (!func.has() && idx.has()) {
            return on_idx(env->env, v->as_table(), std::move(idx));
        } else {
            rfail(base_exc_t::GENERIC,
                  "Cannot provide both a function and an index to %s.",
                  name());
        }
    }
    virtual bool uses_idx() const = 0;
    virtual scoped_ptr_t<val_t> on_idx(
        env_t *env, counted_t<table_t> tbl, scoped_ptr_t<val_t> idx) const = 0;
};

template<class T>
class unindexable_map_acc_term_t : public map_acc_term_t<T> {
protected:
    template<class... Args> unindexable_map_acc_term_t(Args... args)
        : map_acc_term_t<T>(args...) { }
private:
    virtual bool uses_idx() const { return false; }
    virtual scoped_ptr_t<val_t> on_idx(
        env_t *, counted_t<table_t>, scoped_ptr_t<val_t>) const {
        rfail(base_exc_t::GENERIC, "Cannot call %s on an index.", this->name());
    }
};
class sum_term_t : public unindexable_map_acc_term_t<sum_wire_func_t> {
public:
    template<class... Args> sum_term_t(Args... args)
        : unindexable_map_acc_term_t<sum_wire_func_t>(args...) { }
private:
    virtual const char *name() const { return "sum"; }
};
class avg_term_t : public unindexable_map_acc_term_t<avg_wire_func_t> {
public:
    template<class... Args> avg_term_t(Args... args)
        : unindexable_map_acc_term_t<avg_wire_func_t>(args...) { }
private:
    virtual const char *name() const { return "avg"; }
};

template<class T>
class indexable_map_acc_term_t : public map_acc_term_t<T> {
protected:
    template<class... Args> indexable_map_acc_term_t(Args... args)
        : map_acc_term_t<T>(args...) { }
    virtual ~indexable_map_acc_term_t() { }
private:
    virtual bool uses_idx() const { return true; }
    virtual scoped_ptr_t<val_t> on_idx(
        env_t *env, counted_t<table_t> tbl, scoped_ptr_t<val_t> idx) const {
        boost::optional<std::string> idx_str;
        if (idx.has()) {
            idx_str = idx->as_str().to_std();
        }
        counted_t<table_slice_t> slice(new table_slice_t(tbl, idx_str, sorting()));
        return term_t::new_val(single_selection_t::from_slice(
            env,
            term_t::backtrace(),
            slice,
            strprintf("`%s` found no entries in the specified index.",
                      this->name())));
    }
    virtual sorting_t sorting() const = 0;
};

class min_term_t : public indexable_map_acc_term_t<min_wire_func_t> {
public:
    template<class... Args> min_term_t(Args... args)
        : indexable_map_acc_term_t<min_wire_func_t>(args...) { }
private:
    virtual sorting_t sorting() const { return sorting_t::ASCENDING; }
    virtual const char *name() const { return "min"; }
};
class max_term_t : public indexable_map_acc_term_t<max_wire_func_t> {
public:
    template<class... Args> max_term_t(Args... args)
        : indexable_map_acc_term_t<max_wire_func_t>(args...) { }
private:
    virtual sorting_t sorting() const { return sorting_t::DESCENDING; }
    virtual const char *name() const { return "max"; }
};

class count_term_t : public grouped_seq_op_term_t {
public:
    count_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(1, 2)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args,
                                          eval_flags_t) const {
        scoped_ptr_t<val_t> v0 = args->arg(env, 0);
        if (args->num_args() == 1) {
            if (v0->get_type().is_convertible(val_t::type_t::DATUM)) {
                datum_t d = v0->as_datum();
                if (d.get_type() == datum_t::R_BINARY) {
                    return new_val(datum_t(
                       safe_to_double(d.as_binary().size())));
                }
            }
            return v0->as_seq(env->env)
                ->run_terminal(env->env, count_wire_func_t());
        } else {
            scoped_ptr_t<val_t> v1 = args->arg(env, 1);
            if (v1->get_type().is_convertible(val_t::type_t::FUNC)) {
                counted_t<datum_stream_t> stream = v0->as_seq(env->env);
                stream->add_transformation(
                        filter_wire_func_t(v1->as_func(), boost::none),
                        backtrace());
                return stream->run_terminal(env->env, count_wire_func_t());
            } else {
                counted_t<const func_t> f =
                    new_eq_comparison_func(v1->as_datum(), backtrace());
                counted_t<datum_stream_t> stream = v0->as_seq(env->env);
                stream->add_transformation(
                        filter_wire_func_t(f, boost::none), backtrace());

                return stream->run_terminal(env->env, count_wire_func_t());
            }
        }
    }
    virtual const char *name() const { return "count"; }
};

class map_term_t : public grouped_seq_op_term_t {
public:
    map_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(2, -1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        std::vector<counted_t<datum_stream_t> > streams;
        streams.reserve(args->num_args() - 1);
        for (size_t i = 0; i < args->num_args() - 1; ++i) {
            streams.push_back(args->arg(env, i)->as_seq(env->env));
        }

        counted_t<const func_t> func =
            args->arg(env, args->num_args() - 1)->as_func();
        boost::optional<std::size_t> func_arity = func->arity();
        if (!!func_arity) {
            rcheck(func_arity.get() == args->num_args() - 1,
                   base_exc_t::GENERIC,
                   strprintf("The function passed to `map` expects %zu argument%s, "
                             "but %zu sequence%s found.",
                             func_arity.get(), (func_arity.get() == 1 ? "" : "s"),
                             args->num_args() - 1, (args->num_args() == 2 ? " was" : "s were")));
        }

        if (args->num_args() == 2) {
            streams.front()->add_transformation(
                    map_wire_func_t(std::move(func)), backtrace());
            return new_val(env->env, streams.front());
        } else {
            counted_t<datum_stream_t> map_stream = make_counted<map_datum_stream_t>(
                std::move(streams), std::move(func), backtrace());
            return new_val(env->env, map_stream);
        }
    }
    virtual const char *name() const { return "map"; }
};

class concatmap_term_t : public grouped_seq_op_term_t {
public:
    concatmap_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(2)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<datum_stream_t> stream = args->arg(env, 0)->as_seq(env->env);
        stream->add_transformation(
                concatmap_wire_func_t(args->arg(env, 1)->as_func()), backtrace());
        return new_val(env->env, stream);
    }
    virtual const char *name() const { return "concatmap"; }
};

class group_term_t : public grouped_seq_op_term_t {
public:
    group_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(1, -1), optargspec_t({"index", "multi"})) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        std::vector<counted_t<const func_t> > funcs;
        funcs.reserve(args->num_args() - 1);
        for (size_t i = 1; i < args->num_args(); ++i) {
            funcs.push_back(args->arg(env, i)->as_func(GET_FIELD_SHORTCUT));
        }

        counted_t<datum_stream_t> seq;
        bool append_index = false;
        if (scoped_ptr_t<val_t> index = args->optarg(env, "index")) {
            std::string index_str = index->as_str().to_std();
            counted_t<table_t> tbl = args->arg(env, 0)->as_table();
            counted_t<table_slice_t> slice;
            if (index_str == tbl->get_pkey()) {
                auto field = index->as_datum();
                funcs.push_back(new_get_field_func(field, backtrace()));
                slice = make_counted<table_slice_t>(tbl, index_str);
            } else {
                slice = make_counted<table_slice_t>(
                    tbl, index_str, sorting_t::ASCENDING);
                append_index = true;
            }
            r_sanity_check(slice.has());
            seq = slice->as_seq(env->env, backtrace());
        } else {
            seq = args->arg(env, 0)->as_seq(env->env);
        }

        rcheck((funcs.size() + append_index) != 0, base_exc_t::GENERIC,
               "Cannot group by nothing.");

        bool multi = false;
        if (scoped_ptr_t<val_t> multi_val = args->optarg(env, "multi")) {
            multi = multi_val->as_bool();
        }

        seq->add_grouping(group_wire_func_t(std::move(funcs),
                                            append_index,
                                            multi),
                          backtrace());

        return new_val(env->env, seq);
    }
    virtual const char *name() const { return "group"; }
};

class filter_term_t : public grouped_seq_op_term_t {
public:
    filter_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(2), optargspec_t({"default"})),
          default_filter_term(lazy_literal_optarg(env, "default")) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        scoped_ptr_t<val_t> v0 = args->arg(env, 0);
        scoped_ptr_t<val_t> v1 = args->arg(env, 1, LITERAL_OK);
        counted_t<const func_t> f = v1->as_func(CONSTANT_SHORTCUT);
        boost::optional<wire_func_t> defval;
        if (default_filter_term.has()) {
            defval = wire_func_t(default_filter_term->eval_to_func(env->scope));
        }

        if (v0->get_type().is_convertible(val_t::type_t::SELECTION)) {
            counted_t<selection_t> ts = v0->as_selection(env->env);
            ts->seq->add_transformation(filter_wire_func_t(f, defval), backtrace());
            return new_val(ts);
        } else {
            counted_t<datum_stream_t> stream = v0->as_seq(env->env);
            stream->add_transformation(
                    filter_wire_func_t(f, defval), backtrace());
            return new_val(env->env, stream);
        }
    }

    virtual const char *name() const { return "filter"; }

    counted_t<func_term_t> default_filter_term;
};

class reduce_term_t : public grouped_seq_op_term_t {
public:
    reduce_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        grouped_seq_op_term_t(env, term, argspec_t(2), optargspec_t({ "base" })) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        return args->arg(env, 0)->as_seq(env->env)->run_terminal(
            env->env, reduce_wire_func_t(args->arg(env, 1)->as_func()));
    }
    virtual const char *name() const { return "reduce"; }
};

struct rcheck_spec_visitor_t : public pb_rcheckable_t,
                               public boost::static_visitor<void> {
    template<class... Args>
    explicit rcheck_spec_visitor_t(env_t *_env, Args &&... args)
        : pb_rcheckable_t(std::forward<Args...>(args)...), env(_env) { }
    void operator()(const changefeed::keyspec_t::range_t &) const { }
    void operator()(const changefeed::keyspec_t::limit_t &spec) const {
        rcheck(spec.limit <= env->limits().array_size_limit(), base_exc_t::GENERIC,
               strprintf(
                   "Array size limit `%zu` exceeded.  (`.limit(X).changes()` is illegal "
                   "if X is larger than the array size limit.)",
                   env->limits().array_size_limit()));
    }
    void operator()(const changefeed::keyspec_t::point_t &) const { }
    env_t *env;
};

class changes_term_t : public op_term_t {
public:
    changes_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        scoped_ptr_t<val_t> v = args->arg(env, 0);
        if (v->get_type().is_convertible(val_t::type_t::SELECTION)) {
            counted_t<selection_t> selection = v->as_selection(env->env);
            counted_t<table_t> tbl = selection->table;
            counted_t<datum_stream_t> seq = selection->seq;
            auto spec = seq->get_change_spec();
            boost::apply_visitor(
                rcheck_spec_visitor_t(env->env, backtrace()),
                spec.spec);
            return new_val(
                env->env,
                tbl->tbl->read_changes(
                    env->env, std::move(spec), backtrace(), tbl->display_name()));
        } else if (v->get_type().is_convertible(val_t::type_t::SINGLE_SELECTION)) {
            return new_val(
                env->env, v->as_single_selection()->read_changes());
        }
        auto selection = v->as_selection(env->env);
        rfail(base_exc_t::GENERIC,
              ".changes() not yet supported on range selections");
    }
    virtual const char *name() const { return "changes"; }
};

class between_term_t : public bounded_op_term_t {
public:
    between_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : bounded_op_term_t(env, term, argspec_t(3), optargspec_t({"index"})) { }
private:
    virtual scoped_ptr_t<val_t>
    eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_slice_t> tbl_slice = args->arg(env, 0)->as_table_slice();
        bool left_open = is_left_open(env, args);
        datum_t lb = args->arg(env, 1)->as_datum();
        if (lb.get_type() == datum_t::R_NULL) {
            lb.reset();
        }
        bool right_open = is_right_open(env, args);
        datum_t rb = args->arg(env, 2)->as_datum();
        if (rb.get_type() == datum_t::R_NULL) {
            rb.reset();
        }

        if (lb.has() && rb.has()) {
            // This reql_version will always be LATEST, because this function is not
            // deterministic, but whatever.
            if (lb.compare_gt(env->env->reql_version(), rb) ||
                ((left_open || right_open) && lb == rb)) {
                counted_t<datum_stream_t> ds
                    =  make_counted<array_datum_stream_t>(datum_t::empty_array(),
                                                          backtrace());
                return new_val(make_counted<selection_t>(tbl_slice->get_tbl(), ds));
            }
        }

        scoped_ptr_t<val_t> sindex = args->optarg(env, "index");
        std::string idx;
        if (sindex.has()) {
            idx = sindex->as_str().to_std();
        } else {
            boost::optional<std::string> old_idx = tbl_slice->get_idx();
            idx = old_idx ? *old_idx : tbl_slice->get_tbl()->get_pkey();
        }
        return new_val(
            tbl_slice->with_bounds(
                idx,
                datum_range_t(
                    lb, left_open ? key_range_t::open : key_range_t::closed,
                    rb, right_open ? key_range_t::open : key_range_t::closed)));
    }
    virtual const char *name() const { return "between"; }

    protob_t<Term> filter_func;
};

class union_term_t : public op_term_t {
public:
    union_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(0, -1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        std::vector<counted_t<datum_stream_t> > streams;
        for (size_t i = 0; i < args->num_args(); ++i) {
            streams.push_back(args->arg(env, i)->as_seq(env->env));
        }
        counted_t<datum_stream_t> union_stream
            = make_counted<union_datum_stream_t>(std::move(streams), backtrace());
        return new_val(env->env, union_stream);
    }
    virtual const char *name() const { return "union"; }
};

class zip_term_t : public op_term_t {
public:
    zip_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<datum_stream_t> stream = args->arg(env, 0)->as_seq(env->env);
        stream->add_transformation(zip_wire_func_t(), backtrace());
        return new_val(env->env, stream);
    }
    virtual const char *name() const { return "zip"; }
};

class range_term_t : public op_term_t {
public:
    range_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(0, 2)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        bool is_infinite = false;
        int64_t start = 0, stop = 0;

        if (args->num_args() == 0) {
            is_infinite = true;
        } else if (args->num_args() == 1) {
            stop = args->arg(env, 0)->as_int();
        } else {
            r_sanity_check(args->num_args() == 2);
            start = args->arg(env, 0)->as_int();
            stop = args->arg(env, 1)->as_int();
        }

        return new_val(env->env, make_counted<range_datum_stream_t>(
            is_infinite, start, stop, backtrace()));
    }
    virtual const char *name() const { return "range"; }
};

counted_t<term_t> make_between_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<between_term_t>(env, term);
}
counted_t<term_t> make_changes_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<changes_term_t>(env, term);
}
counted_t<term_t> make_reduce_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<reduce_term_t>(env, term);
}
counted_t<term_t> make_map_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<map_term_t>(env, term);
}
counted_t<term_t> make_filter_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<filter_term_t>(env, term);
}
counted_t<term_t> make_concatmap_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<concatmap_term_t>(env, term);
}
counted_t<term_t> make_group_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<group_term_t>(env, term);
}
counted_t<term_t> make_count_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<count_term_t>(env, term);
}
counted_t<term_t> make_avg_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<avg_term_t>(env, term);
}
counted_t<term_t> make_sum_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<sum_term_t>(env, term);
}
counted_t<term_t> make_min_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<min_term_t>(env, term);
}
counted_t<term_t> make_max_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<max_term_t>(env, term);
}
counted_t<term_t> make_union_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<union_term_t>(env, term);
}
counted_t<term_t> make_zip_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<zip_term_t>(env, term);
}

counted_t<term_t> make_range_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<range_term_t>(env, term);
}

} // namespace ql
