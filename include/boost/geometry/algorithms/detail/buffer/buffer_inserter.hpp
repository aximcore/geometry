// Boost.Geometry (aka GGL, Generic Geometry Library)

// Copyright (c) 2012-2014 Barend Gehrels, Amsterdam, the Netherlands.

// Use, modification and distribution is subject to the Boost Software License,
// Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_GEOMETRY_ALGORITHMS_DETAIL_BUFFER_BUFFER_INSERTER_HPP
#define BOOST_GEOMETRY_ALGORITHMS_DETAIL_BUFFER_BUFFER_INSERTER_HPP

#include <cstddef>
#include <iterator>

#include <boost/numeric/conversion/cast.hpp>

#include <boost/range.hpp>

#include <boost/geometry/core/exterior_ring.hpp>
#include <boost/geometry/core/interior_rings.hpp>

#include <boost/geometry/util/math.hpp>

#include <boost/geometry/strategies/buffer.hpp>
#include <boost/geometry/strategies/cartesian/buffer_side.hpp>
#include <boost/geometry/strategies/side.hpp>
#include <boost/geometry/algorithms/detail/buffer/buffered_piece_collection.hpp>
#include <boost/geometry/algorithms/detail/buffer/line_line_intersection.hpp>
#include <boost/geometry/algorithms/detail/buffer/parallel_continue.hpp>

#include <boost/geometry/algorithms/simplify.hpp>


namespace boost { namespace geometry
{

#ifndef DOXYGEN_NO_DETAIL
namespace detail { namespace buffer
{


template <typename RingOutput>
struct buffer_range
{
    typedef typename point_type<RingOutput>::type output_point_type;
    typedef typename coordinate_type<RingOutput>::type coordinate_type;

    template
    <
        typename Collection,
        typename Point,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline
    void add_join(Collection& collection,
            int phase,
            Point const& penultimate_input,
            Point const& previous_input,
            output_point_type const& prev_perp1,
            output_point_type const& prev_perp2,
            Point const& input,
            output_point_type const& perp1,
            output_point_type const& perp2,
            strategy::buffer::buffer_side_selector side,
            DistanceStrategy const& distance,
            JoinStrategy const& join_strategy,
            EndStrategy const& end_strategy,
            RobustPolicy const& )
    {
        output_point_type intersection_point;

        strategy::buffer::join_selector join
                = get_join_type(penultimate_input, previous_input, input);
        if (join == strategy::buffer::join_convex)
        {
            // Calculate the intersection-point formed by the two sides.
            // It might be that the two sides are not convex, but continue
            // or spikey, we then change the join-type
            join = line_line_intersection::apply(
                        perp1, perp2, prev_perp1, prev_perp2,
                        intersection_point);

        }
        switch(join)
        {
            case strategy::buffer::join_continue :
                // No join, we get two consecutive sides
                return;
            case strategy::buffer::join_concave :
                collection.add_piece(strategy::buffer::buffered_concave,
                        previous_input, prev_perp2, perp1);
                return;
            case strategy::buffer::join_spike :
                //if (phase == 0) avoid duplicate joins at spikes? this still causes other issues
                {
                    // For linestrings, only add spike at one side to avoid
                    // duplicates
                    std::vector<output_point_type> range_out;
                    end_strategy.apply(penultimate_input, prev_perp2, previous_input, perp1, side, distance, range_out);
                    collection.add_endcap(end_strategy, range_out, previous_input);
                }
                return;
            case strategy::buffer::join_convex :
                break; // All code below handles this
        }

        // The corner is convex, we create a join
        // TODO - try to avoid a separate vector, add the piece directly
        std::vector<output_point_type> range_out;
        join_strategy.apply(intersection_point,
                    previous_input, prev_perp2, perp1,
                    distance.apply(previous_input, input, side),
                    range_out);
        collection.add_piece(strategy::buffer::buffered_join,
                previous_input, range_out);
    }

    static inline strategy::buffer::join_selector get_join_type(
            output_point_type const& p0,
            output_point_type const& p1,
            output_point_type const& p2)
    {
        typedef typename strategy::side::services::default_strategy
            <
                typename cs_tag<output_point_type>::type
            >::type side_strategy;

        int const side = side_strategy::apply(p0, p1, p2);
        return side == -1 ? strategy::buffer::join_convex
            :  side == 1  ? strategy::buffer::join_concave
            :  parallel_continue
                    (
                        get<0>(p2) - get<0>(p1),
                        get<1>(p2) - get<1>(p1),
                        get<0>(p1) - get<0>(p0),
                        get<1>(p1) - get<1>(p0)
                    )  ? strategy::buffer::join_continue
            : strategy::buffer::join_spike;
    }

    template
    <
        typename Collection,
        typename Iterator,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void iterate(Collection& collection,
                int phase, // 0/1 for left/right of rings. For polygons: 0
                Iterator begin, Iterator end,
                strategy::buffer::buffer_side_selector side,
                DistanceStrategy const& distance_strategy,
                JoinStrategy const& join_strategy,
                EndStrategy const& end_strategy,
                RobustPolicy const& robust_policy,
                output_point_type& first_p1,
                output_point_type& first_p2,
                output_point_type& last_p1,
                output_point_type& last_p2)
    {
        typedef typename std::iterator_traits
        <
            Iterator
        >::value_type point_type;

        typedef typename robust_point_type
        <
            point_type,
            RobustPolicy
        >::type robust_point_type;

        robust_point_type previous_robust_input;
        point_type second_point, penultimate_point, ultimate_point; // last two points from begin/end

        /*
         * last.p1    last.p2  these are the "previous (last) perpendicular points"
         * --------------
         * |            |
         * *------------*____  <- *prev
         * pup          |    | p1           "current perpendicular point 1"
         *              |    |
         *              |    |       this forms a "side", a side is a piece
         *              |    |
         *              *____| p2
         *
         *              ^
         *             *it
         *
         * pup: penultimate_point
         */

        bool first = true;

        Iterator it = begin;

        geometry::recalculate(previous_robust_input, *begin, robust_policy);

        std::vector<output_point_type> generated_side;
        generated_side.reserve(2);

        for (Iterator prev = it++; it != end; ++it)
        {
            robust_point_type robust_input;
            geometry::recalculate(robust_input, *it, robust_policy);
            // Check on equality - however, if input is simplified, this is highly
            // unlikely (though possible by rescaling)
            if (! detail::equals::equals_point_point(previous_robust_input, robust_input))
            {
                generated_side.clear();
                strategy::buffer::buffer_side::apply(*prev,
                    *it, side, distance_strategy, generated_side);

                if (! first)
                {
                     add_join(collection, phase,
                            penultimate_point,
                            *prev, last_p1, last_p2,
                            *it, generated_side.front(), generated_side.back(),
                            side,
                            distance_strategy, join_strategy, end_strategy,
                            robust_policy);
                }

                collection.add_piece(strategy::buffer::buffered_segment,
                    *prev, *it, generated_side, first);

                penultimate_point = *prev;
                ultimate_point = *it;
                last_p1 = generated_side.front();
                last_p2 = generated_side.back();
                prev = it;
                if (first)
                {
                    first = false;
                    second_point = *it;
                    first_p1 = generated_side.front();
                    first_p2 = generated_side.back();
                }
            }
            previous_robust_input = robust_input;
        }
    }
};



template
<
    typename Point,
    typename RingOutput
>
struct buffer_point
{
    typedef typename point_type<RingOutput>::type output_point_type;
    typedef typename coordinate_type<RingOutput>::type coordinate_type;

    typedef typename geometry::select_most_precise
        <
            typename geometry::select_most_precise
                <
                    typename geometry::coordinate_type<Point>::type,
                    typename geometry::coordinate_type<output_point_type>::type
                >::type,
            double
        >::type promoted_type;


    template <typename RangeOut>
    static inline void generate_points(Point const& point,
                promoted_type const& buffer_distance,
                RangeOut& range_out)
    {

        promoted_type two = 2.0;
        promoted_type two_pi = two * geometry::math::pi<promoted_type>();
        int point_buffer_count = 88; // 88 gives now fixed problem (collinear opposite / robustness. TODO: make this value flexible

        promoted_type diff = two_pi / promoted_type(point_buffer_count);
        promoted_type a = 0;

        output_point_type first;
        for (int i = 0; i < point_buffer_count; i++, a -= diff)
        {
            output_point_type p;
            set<0>(p, get<0>(point) + buffer_distance * cos(a));
            set<1>(p, get<1>(point) + buffer_distance * sin(a));
            range_out.push_back(p);
            if (i == 0)
            {
                first = p;
            }
        }

        // Close it:
        range_out.push_back(first);
    }


    template
    <
        typename Collection,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void generate_circle(Point const& point,
                Collection& collection,
                DistanceStrategy const& distance,
                JoinStrategy const& ,
                EndStrategy const& ,
                RobustPolicy const& )
    {
        std::vector<output_point_type> range_out;

        generate_points(point,
            distance.apply(point, point, strategy::buffer::buffer_side_left),
            range_out);

        collection.add_piece(strategy::buffer::buffered_circle, range_out, false);
    }
};

template
<
    typename Multi,
    typename PolygonOutput,
    typename Policy
>
struct buffer_multi
{
    template
    <
        typename Collection,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void apply(Multi const& multi,
            Collection& collection,
            DistanceStrategy const& distance,
            JoinStrategy const& join_strategy,
            EndStrategy const& end_strategy,
            RobustPolicy const& robust_policy)
    {
        for (typename boost::range_iterator<Multi const>::type
                it = boost::begin(multi);
            it != boost::end(multi);
            ++it)
        {
            Policy::apply(*it, collection, distance, join_strategy, end_strategy, robust_policy);
        }
    }
};

struct visit_pieces_default_policy
{
    template <typename Collection>
    static inline void apply(Collection const&, int)
    {}
};

}} // namespace detail::buffer
#endif // DOXYGEN_NO_DETAIL


#ifndef DOXYGEN_NO_DISPATCH
namespace dispatch
{

template
<
    typename Tag,
    typename RingInput,
    typename RingOutput
>
struct buffer_inserter
{};



template
<
    typename Point,
    typename RingOutput
>
struct buffer_inserter<point_tag, Point, RingOutput>
    : public detail::buffer::buffer_point<Point, RingOutput>
{
    template
    <
        typename Collection,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void apply(Point const& point, Collection& collection,
            DistanceStrategy const& distance,
            JoinStrategy const& join_strategy,
            EndStrategy const& end_strategy,
            RobustPolicy const& robust_policy)
    {
        collection.start_new_ring();
            typedef detail::buffer::buffer_point<Point, RingOutput> base;
        base::generate_circle(point, collection, distance, join_strategy, end_strategy, robust_policy);
    }
};


template
<
    typename RingInput,
    typename RingOutput
>
struct buffer_inserter<ring_tag, RingInput, RingOutput>
{
    typedef typename point_type<RingOutput>::type output_point_type;

    template
    <
        typename Collection,
        typename Iterator,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void iterate(Collection& collection,
                Iterator begin, Iterator end,
                strategy::buffer::buffer_side_selector side,
                DistanceStrategy const& distance_strategy,
                JoinStrategy const& join_strategy,
                EndStrategy const& end_strategy,
                RobustPolicy const& robust_policy)
    {
        output_point_type first_p1, first_p2, last_p1, last_p2;

        typedef detail::buffer::buffer_range<RingOutput> buffer_range;

        buffer_range::iterate(collection, 0, begin, end,
                side,
                distance_strategy, join_strategy, end_strategy, robust_policy,
                first_p1, first_p2, last_p1, last_p2);

        // Generate closing join
        buffer_range::add_join(collection, 0,
            *(end - 2),
            *(end - 1), last_p1, last_p2,
            *(begin + 1), first_p1, first_p2,
            side,
            distance_strategy, join_strategy, end_strategy,
            robust_policy);

        // Buffer is closed automatically by last closing corner
        // (OPEN IT FOR OPEN POLYGONS - TODO)
    }

    template
    <
        typename Collection,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void apply(RingInput const& ring,
            Collection& collection,
            DistanceStrategy const& distance,
            JoinStrategy const& join_strategy,
            EndStrategy const& end_strategy,
            RobustPolicy const& robust_policy)
    {
        if (boost::size(ring) > 3)
        {
            // We have to simplify the ring before to avoid very small-scaled
            // features in the original (convex/concave/convex) being enlarged
            // in a very large scale and causing issues (IP's within pieces).
            // This might be reconsidered later. Simplifying with a very small
            // distance (1%% of the buffer) will never be visible in the result,
            // if it is using round joins. For miter joins they are even more
            // sensitive to small scale input features, however the result will
            // look better.
            // It also get rid of duplicate points
            RingOutput simplified;
            geometry::simplify(ring, simplified, distance.simplify_distance());

            if (distance.negative())
            {
                // Walk backwards (rings will be reversed afterwards)
                // It might be that this will be changed later.
                // TODO: decide this.
                iterate(collection, boost::rbegin(simplified), boost::rend(simplified),
                        strategy::buffer::buffer_side_right,
                        distance, join_strategy, end_strategy, robust_policy);
            }
            else
            {
                iterate(collection, boost::begin(simplified), boost::end(simplified),
                        strategy::buffer::buffer_side_left,
                        distance, join_strategy, end_strategy, robust_policy);
            }

        }
    }
};


template
<
    typename Linestring,
    typename Polygon
>
struct buffer_inserter<linestring_tag, Linestring, Polygon>
{
    typedef typename ring_type<Polygon>::type output_ring_type;
    typedef typename point_type<output_ring_type>::type output_point_type;
    typedef typename point_type<Linestring>::type input_point_type;

    template <typename DistanceStrategy>
    static inline output_point_type first_perpendicular_point(
        input_point_type const& p1, input_point_type const& p2,
        DistanceStrategy const& distance_strategy)
    {
        std::vector<output_point_type> generated_side;
        strategy::buffer::buffer_side::apply(p1, p2,
                strategy::buffer::buffer_side_right,
                distance_strategy, generated_side);
        return generated_side.front();
    }

    template
    <
        typename Collection,
        typename Iterator,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void iterate(Collection& collection, int phase,
                Iterator begin, Iterator end,
                strategy::buffer::buffer_side_selector side,
                DistanceStrategy const& distance_strategy,
                JoinStrategy const& join_strategy,
                EndStrategy const& end_strategy,
                RobustPolicy const& robust_policy,
                output_point_type& first_p1)
    {
        input_point_type const& ultimate_point = *(end - 1);
        input_point_type const& penultimate_point = *(end - 2);

        // For the end-cap, we need to have the last perpendicular point on the
        // other side of the linestring. If it is the second pass (right),
        // we have it already from the first phase (left).
        // But for the first pass, we have to generate it
        output_point_type reverse_p1
            = side == strategy::buffer::buffer_side_right
            ? first_p1
            : first_perpendicular_point(ultimate_point, penultimate_point, distance_strategy);

        output_point_type first_p2, last_p1, last_p2;

        detail::buffer::buffer_range<output_ring_type>::iterate(collection,
                phase, begin, end, side,
                distance_strategy, join_strategy, end_strategy, robust_policy,
                first_p1, first_p2, last_p1, last_p2);

        std::vector<output_point_type> range_out;
        end_strategy.apply(penultimate_point, last_p2, ultimate_point, reverse_p1, side, distance_strategy, range_out);
        collection.add_endcap(end_strategy, range_out, ultimate_point);
    }

    template
    <
        typename Collection,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void apply(Linestring const& linestring, Collection& collection,
            DistanceStrategy const& distance,
            JoinStrategy const& join_strategy,
            EndStrategy const& end_strategy,
            RobustPolicy const& robust_policy)
    {
        if (boost::size(linestring) > 1)
        {
            Linestring simplified;
            geometry::simplify(linestring, simplified, distance.simplify_distance());

            collection.start_new_ring();
            output_point_type first_p1;
            iterate(collection, 0, boost::begin(simplified), boost::end(simplified),
                    strategy::buffer::buffer_side_left,
                    distance, join_strategy, end_strategy, robust_policy,
                    first_p1);

            iterate(collection, 1, boost::rbegin(simplified), boost::rend(simplified),
                    strategy::buffer::buffer_side_right,
                    distance, join_strategy, end_strategy, robust_policy,
                    first_p1);
        }

    }
};


template
<
    typename PolygonInput,
    typename PolygonOutput
>
struct buffer_inserter<polygon_tag, PolygonInput, PolygonOutput>
{
private:
    typedef typename ring_type<PolygonInput>::type input_ring_type;
    typedef typename ring_type<PolygonOutput>::type output_ring_type;

    typedef buffer_inserter<ring_tag, input_ring_type, output_ring_type> policy;


    template
    <
        typename Iterator,
        typename Collection,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline
    void iterate(Iterator begin, Iterator end,
            Collection& collection,
            DistanceStrategy const& distance,
            JoinStrategy const& join_strategy,
            EndStrategy const& end_strategy,
            RobustPolicy const& robust_policy)
    {
        for (Iterator it = begin; it != end; ++it)
        {
            collection.start_new_ring();
            policy::apply(*it, collection, distance, join_strategy, end_strategy, robust_policy);
        }
    }

    template
    <
        typename InteriorRings,
        typename Collection,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline
    void apply_interior_rings(InteriorRings const& interior_rings,
            Collection& collection,
            DistanceStrategy const& distance,
            JoinStrategy const& join_strategy,
            EndStrategy const& end_strategy,
            RobustPolicy const& robust_policy)
    {
        iterate(boost::begin(interior_rings), boost::end(interior_rings),
            collection, distance, join_strategy, end_strategy, robust_policy);
    }

public:
    template
    <
        typename Collection,
        typename DistanceStrategy,
        typename JoinStrategy,
        typename EndStrategy,
        typename RobustPolicy
    >
    static inline void apply(PolygonInput const& polygon,
            Collection& collection,
            DistanceStrategy const& distance,
            JoinStrategy const& join_strategy,
            EndStrategy const& end_strategy,
            RobustPolicy const& robust_policy)
    {
        {
            collection.start_new_ring();
            policy::apply(exterior_ring(polygon), collection,
                    distance, join_strategy, end_strategy, robust_policy);
        }

        apply_interior_rings(interior_rings(polygon),
            collection, distance, join_strategy, end_strategy, robust_policy);
    }
};


template
<
    typename Multi,
    typename PolygonOutput
>
struct buffer_inserter<multi_tag, Multi, PolygonOutput>
    : public detail::buffer::buffer_multi
             <
                Multi,
                PolygonOutput,
                dispatch::buffer_inserter
                <
                    typename single_tag_of
                                <
                                    typename tag<Multi>::type
                                >::type,
                    typename boost::range_value<Multi const>::type,
                    typename geometry::ring_type<PolygonOutput>::type
                >
            >
{};


} // namespace dispatch
#endif // DOXYGEN_NO_DISPATCH

template
<
    typename GeometryOutput,
    typename GeometryInput,
    typename OutputIterator,
    typename DistanceStrategy,
    typename JoinStrategy,
    typename EndStrategy,
    typename RobustPolicy,
    typename VisitPiecesPolicy
>
inline void buffer_inserter(GeometryInput const& geometry_input, OutputIterator out,
        DistanceStrategy const& distance_strategy,
        JoinStrategy const& join_strategy,
        EndStrategy const& end_strategy,
        RobustPolicy const& robust_policy,
        VisitPiecesPolicy& visit_pieces_policy
    )
{
    typedef detail::buffer::buffered_piece_collection
    <
        typename geometry::ring_type<GeometryOutput>::type,
        RobustPolicy
    > collection_type;
    collection_type collection(robust_policy);
    collection_type const& const_collection = collection;

    dispatch::buffer_inserter
        <
            typename tag_cast
                <
                    typename tag<GeometryInput>::type,
                    multi_tag
                >::type,
            GeometryInput,
            GeometryOutput
        >::apply(geometry_input, collection, distance_strategy, join_strategy, end_strategy, robust_policy);

    collection.get_turns(geometry_input, distance_strategy);

    // Visit the piece collection. This does nothing (by default), but
    // optionally a debugging tool can be attached (e.g. console or svg),
    // or the piece collection can be unit-tested
    // phase 0: turns (before discarded)
    visit_pieces_policy.apply(const_collection, 0);

    collection.discard_rings();
    collection.discard_turns();
    collection.enrich();
    collection.traverse();

    if (distance_strategy.negative()
        && boost::is_same
            <
                typename tag_cast<typename tag<GeometryInput>::type, areal_tag>::type,
                areal_tag
            >::type::value)
    {
        collection.reverse();
    }

    collection.template assign<GeometryOutput>(out);

    // Visit collection again
    // phase 1: rings (after discarding and traversing)
    visit_pieces_policy.apply(const_collection, 1);
}

template
<
    typename GeometryOutput,
    typename GeometryInput,
    typename OutputIterator,
    typename DistanceStrategy,
    typename JoinStrategy,
    typename EndStrategy,
    typename RobustPolicy
>
inline void buffer_inserter(GeometryInput const& geometry_input, OutputIterator out,
        DistanceStrategy const& distance_strategy,
        JoinStrategy const& join_strategy,
        EndStrategy const& end_strategy,
        RobustPolicy const& robust_policy)
{
    detail::buffer::visit_pieces_default_policy visitor;
    buffer_inserter<GeometryOutput>(geometry_input, out,
        distance_strategy, join_strategy, end_strategy,
        robust_policy, visitor);
}

}} // namespace boost::geometry

#endif // BOOST_GEOMETRY_ALGORITHMS_DETAIL_BUFFER_BUFFER_INSERTER_HPP
