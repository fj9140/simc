// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simcraft.h"

// ==========================================================================
// Simulator
// ==========================================================================

// sim_t::sim_t =============================================================

sim_t::sim_t( sim_t* p ) : 
  parent(p), rng(0), event_list(0), free_list(0), player_list(0), active_player(0),
  lag(0), pet_lag(0), channel_penalty(0), gcd_penalty(0), reaction_time(0.5), 
  regen_periodicity(1.0), current_time(0), max_time(0),
  events_remaining(0), max_events_remaining(0), 
  events_processed(0), total_events_processed(0),
  seed(0), id(0), iterations(1), current_iteration(0), threads(0),
  potion_sickness(1), average_dmg(1), log(0), debug(0), timestamp(1), sfmt(1),
  raid_dps(0), total_dmg(0), total_seconds(0), elapsed_cpu_seconds(0), merge_ignite(0),
  output_file(stdout), html_file(0), wiki_file(0), thread_handle(0)
{
  patch_str = "3.0.3";

  for( int i=0; i < RESOURCE_MAX; i++ ) 
  {
    infinite_resource[ i ] = false;
  }

  target  = new  target_t( this );
  report  = new  report_t( this );
  scaling = new scaling_t( this );

  if( parent ) 
  {
    // Import the config file
    option_t::parse( this, parent -> argc, parent -> argv );

    // Inherit 'delta_gear' settings from parent because these may be set outside of the config file
    gear_delta = parent -> gear_delta;
  }
}

// sim_t::~sim_t ============================================================

sim_t::~sim_t()
{
  flush_events();

  while( player_t* p = player_list )
  {
    player_list = p -> next;
    delete p;
  }

  while( event_t* e = free_list )
  {
    free_list = e -> next;
    event_t::deallocate( e );
  }

  if( rng     ) delete rng;
  if( target  ) delete target;
  if( report  ) delete report;
  if( scaling ) delete scaling;
  
  int num_children = children.size();
  for( int i=0; i < num_children; i++ ) 
  {
    delete children[ i ];
  }
}

// sim_t::add_event ==========================================================

void sim_t::add_event( event_t* e,
		       double   delta_time )
{
  assert( delta_time >= 0 );
  e -> time = current_time + delta_time;
  e -> id   = ++id;
  
  event_t** prev = &event_list;
  while( (*prev) && (*prev) -> time < e -> time ) prev = &( (*prev) -> next );
  e -> next = *prev;
  *prev = e;
  events_remaining++;
  if( events_remaining > max_events_remaining ) max_events_remaining = events_remaining;
  if( debug ) report_t::log( this, "Add Event: %s %.2f %d", e -> name, e -> time, e -> id );
}

// sim_t::reschedule_event ====================================================

void sim_t::reschedule_event( event_t* e )
{
   if( debug ) report_t::log( this, "Reschedule Event: %s %d", e -> name, e -> id );
   add_event( e, ( e -> reschedule_time - current_time ) );
   e -> reschedule_time = 0;
}

// sim_t::next_event ==========================================================

event_t* sim_t::next_event()
{
  event_t* e = event_list;
  if( e ) event_list = e -> next;
  return e;
}

// sim_t::combat ==============================================================

void sim_t::combat( int iteration )
{
  if( debug ) report_t::log( this, "Starting Simulator" );

  current_iteration = iteration;

  combat_begin();

  while( event_t* e = next_event() )
  {
    events_remaining--;
    events_processed++;

    current_time = e -> time;

    if( max_time > 0 && current_time > max_time ) 
    {
      if( debug ) report_t::log( this, "MaxTime reached, ending simulation" );     
      delete e;
      break;
    }
    if( target -> initial_health > 0 && target -> current_health <= 0 )
    {
      if( debug ) report_t::log( this, "Target has died, ending simulation" );     
      delete e;
      break;
    }
    if( target -> initial_health == 0 && current_time > ( max_time / 2.0 ) )
    {
      target -> recalculate_health();
    }
    if( e -> canceled ) 
    {
      if( debug ) report_t::log( this, "Canceled event: %s", e -> name );     
    }
    else if( e -> reschedule_time > e -> time )
    {
      reschedule_event( e );
      continue;
    }
    else
    {
      if( debug ) report_t::log( this, "Executing event: %s", e -> name );     
      e -> execute();
      
      if( e -> player ) e -> player -> last_action = current_time;
    }
    delete e;
  }

  combat_end();
}

// sim_t::flush_events ======================================================

void sim_t::flush_events()
{
   if( debug ) report_t::log( this, "Flush Events" );
   while( event_t* e = next_event() )
     delete e;
   events_remaining = 0;
   events_processed = 0;
   id = 0;
}

// sim_t::cancel_events =====================================================

void sim_t::cancel_events( player_t* p )
{
  for( event_t* e = event_list; e; e = e -> next )
  {
    if( e -> player == p ) 
    {
      e -> canceled = 1;
    }
  }
}

// sim_t::reset =============================================================

void sim_t::reset()
{
  if( debug ) report_t::log( this, "Reseting Simulator" );
  current_time = id = 0;
  target -> reset();
  for( player_t* p = player_list; p; p = p -> next )
  {
    p -> reset();
  }
}

// sim_t::combat_begin ======================================================

void sim_t::combat_begin()
{
  if( debug ) report_t::log( this, "Combat Begin" );

  reset();

  for( player_t* p = player_list; p; p = p -> next )
  {
    p -> combat_begin();
  }
  new ( this ) regen_event_t( this );
}

// sim_t::combat_end ========================================================

void sim_t::combat_end()
{
  if( debug ) report_t::log( this, "Combat End" );

  total_seconds += current_time;
  total_events_processed += events_processed;

  flush_events();

  for( player_t* p = player_list; p; p = p -> next )
  {
    p -> combat_end();
  }
}

// sim_t::init ==============================================================

bool sim_t::init()
{
  rng = rng_t::init( sfmt );
  
  total_seconds = 0;

  if( ! patch_str.empty() )
  {
    int8_t arch, version, revision;
    if( 3 != util_t::string_split( patch_str, ".", "i8 i8 i8", &arch, &version, &revision ) )
    {
      fprintf( output_file, "simcraft: Expected format: -patch=#.#.#\n" );
      return false;
    }
    patch.set( arch, version, revision );
  }

  if( pet_lag == 0 ) pet_lag = lag;

  target -> init();

  bool too_quiet = true;

  for( player_t* p = player_list; p; p = p -> next )
  {
    p -> init();
    if( ! p -> quiet ) too_quiet = false;
  }

  if( too_quiet && ! debug ) exit(0);

  return true;
}

// sim_t::analyze ============================================================

struct compare_dps {
  bool operator()( player_t* l, player_t* r ) const
  {
    return l -> dps > r -> dps;
  }
};

struct compare_name {
  bool operator()( player_t* l, player_t* r ) const
  {
    return l -> name_str < r -> name_str;
  }
};

void sim_t::analyze()
{
  if( total_seconds == 0 ) return;

  total_dmg = 0;
  total_seconds /= iterations;

  for( player_t* p = player_list; p; p = p -> next )
  {
    p -> total_dmg = 0;
    p -> total_seconds /= iterations;
    p -> total_waiting /= iterations;
  }

  for( player_t* p = player_list; p; p = p -> next )
  {
    if( p -> quiet ) continue;

    players_by_rank.push_back( p );
    players_by_name.push_back( p );

    std::vector<stats_t*> stats_list;

    for( stats_t* s = p -> stats_list; s; s = s -> next )
    {
      stats_list.push_back( s );
    }

    for( pet_t* pet = p -> pet_list; pet; pet = pet -> next_pet )
    {
      for( stats_t* s = pet -> stats_list; s; s = s -> next )
      {
	stats_list.push_back( s );
      }
    }

    int num_stats = stats_list.size();

    for( int i=0; i < num_stats; i++ )
    {
      stats_t* s = stats_list[ i ];

      s -> analyze();
      p -> total_dmg += s -> total_dmg;
    }

    p -> dps = p -> total_dmg / p -> total_seconds;

    // Avoid double-counting of pet damage
    if( ! p -> is_pet() ) total_dmg += p -> total_dmg;

    for( int i=0; i < RESOURCE_MAX; i++ )
    {
      p -> resource_lost  [ i ] /= iterations;
      p -> resource_gained[ i ] /= iterations;
    }

    p -> dpr = p -> total_dmg / p -> resource_lost[ p -> primary_resource() ];

    p -> rps_loss = p -> resource_lost  [ p -> primary_resource() ] / p -> total_seconds;
    p -> rps_gain = p -> resource_gained[ p -> primary_resource() ] / p -> total_seconds;

    for( gain_t* g = p -> gain_list; g; g = g -> next )
    {
      g -> amount /= iterations;
    }

    for( proc_t* proc = p -> proc_list; proc; proc = proc -> next )
    {
      if( proc -> count > 0 ) 
      {
	proc -> count /= iterations;
	proc -> frequency = p -> total_seconds / proc -> count;
      }
    }

    int max_buckets = (int) p -> total_seconds;
    
    p -> timeline_dmg.clear();
    p -> timeline_dps.clear();

    p -> timeline_dmg.insert( p -> timeline_dmg.begin(), max_buckets, 0 );
    p -> timeline_dps.insert( p -> timeline_dps.begin(), max_buckets, 0 );

    for( int i=0; i < num_stats; i++ )
    {
      stats_t* s = stats_list[ i ];

      for( int j=0; ( j < max_buckets ) && ( j < s -> num_buckets ); j++ )
      {
	p -> timeline_dmg[ j ] += s -> timeline_dmg[ j ];
      }
    }

    for( int i=0; i < max_buckets; i++ )
    {
      double window_dmg  = p -> timeline_dmg[ i ];
      int    window_size = 1;

      for( int j=1; ( j <= 10 ) && ( (i-j) >=0 ); j++ )
      {
	window_dmg += p -> timeline_dmg[ i-j ];
	window_size++;
      }
      for( int j=1; ( j <= 10 ) && ( (i+j) < max_buckets ); j++ )
      {
	window_dmg += p -> timeline_dmg[ i+j ];
	window_size++;
      }

      p -> timeline_dps[ i ] = window_dmg / window_size;
    }

    assert( p -> iteration_dps.size() == (size_t) iterations );

    p -> dps_min = 0;
    p -> dps_max = 0;
    p -> dps_std_dev = 0;
    for( int i=0; i < iterations; i++ )
    {
      double i_dps = p -> iteration_dps[ i ];
      if( p -> dps_min == 0 || p -> dps_min > i_dps ) p -> dps_min = i_dps;
      if( p -> dps_max == 0 || p -> dps_max < i_dps ) p -> dps_max = i_dps;
      double delta = i_dps - p -> dps;
      p -> dps_std_dev += delta * delta;
    }
    p -> dps_std_dev /= iterations;
    p -> dps_std_dev = sqrt( p -> dps_std_dev );
    p -> dps_error = 2.0 * p -> dps_std_dev / sqrt( (float) iterations );

    if( ( p -> dps_max - p -> dps_min ) > 0 )
    {
      int num_buckets = 50;
      double min = p -> dps_min - 1;
      double max = p -> dps_max + 1;
      double range = max - min;

      p -> dps_distribution.insert( p -> dps_distribution.begin(), num_buckets, 0 );

      for( int i=0; i < iterations; i++ )
      {
	double i_dps = p -> iteration_dps[ i ];
	int index = (int) ( num_buckets * ( i_dps - min ) / range );
	p -> dps_distribution[ index ]++;
      }
    }
  }

  std::sort( players_by_rank.begin(), players_by_rank.end(), compare_dps()  );
  std::sort( players_by_name.begin(), players_by_name.end(), compare_name() );

  raid_dps = total_dmg / total_seconds;
}

// sim_t::iterate ===========================================================

void sim_t::iterate()
{
  init();
  for( int i=0; i < iterations; i++ )
  {
    combat( i );
  }
  reset();
}

// sim_t::merge =============================================================

void sim_t::merge( sim_t& other_sim )
{
  iterations             += other_sim.iterations;
  total_seconds          += other_sim.total_seconds;
  total_events_processed += other_sim.total_events_processed;

  if( max_events_remaining < other_sim.max_events_remaining ) max_events_remaining = other_sim.max_events_remaining;

  for( player_t* p = player_list; p; p = p -> next )
  {
    player_t* other_p = other_sim.find_player( p -> name() );
    assert( other_p );

    p -> total_seconds += other_p -> total_seconds;
    p -> total_waiting += other_p -> total_waiting;

    for( int i=0; i < other_sim.iterations; i++ )
    {
      p -> iteration_dps.push_back( other_p -> iteration_dps[ i ] );
    }

    for( int i=0; i < RESOURCE_MAX; i++ )
    {
      p -> resource_lost  [ i ] += other_p -> resource_lost  [ i ];
      p -> resource_gained[ i ] += other_p -> resource_gained[ i ];
    }

    for( proc_t* proc = p -> proc_list; proc; proc = proc -> next )
    {
      proc -> merge( other_p -> get_proc( proc -> name_str ) );
    }    

    for( gain_t* gain = p -> gain_list; gain; gain = gain -> next )
    {
      gain -> merge( other_p -> get_gain( gain -> name_str ) );
    }    

    for( stats_t* stats = p -> stats_list; stats; stats = stats -> next )
    {
      stats -> merge( other_p -> get_stats( stats -> name_str ) );
    }    

    for( uptime_t* uptime = p -> uptime_list; uptime; uptime = uptime -> next )
    {
      uptime -> merge( other_p -> get_uptime( uptime -> name_str ) );
    }    
  }  
}

// sim_t::merge =============================================================

void sim_t::merge()
{
  int num_children = children.size();
  
  for( int i=0; i < num_children; i++ )
  {
    sim_t* child = children[ i ];
    thread_t::wait( child );
    merge( *child );
    delete child;
  }

  children.clear();
}

// sim_t::partition =========================================================

void sim_t::partition()
{
  if( threads <= 1 ) return;
  if( iterations < threads ) return;

#if defined( NO_THREADS )
  fprintf( output_file, "simcraft: This executable was built without thread support, please remove 'threads=N' from config file.\n" );
  exit(0);
#endif

  iterations /= threads;

  int num_children = threads - 1;
  children.resize( num_children );

  for( int i=0; i < num_children; i++ )
  {
    sim_t* child = children[ i ] = new sim_t( this );
    child -> iterations /= threads;
  }

  for( int i=0; i < num_children; i++ )
  {
    thread_t::launch( children[ i ] );
  }
}

// sim_t::execute ===========================================================

void sim_t::execute()
{
  time_t start_time = time(0);

  partition();
  iterate();
  merge();
  analyze();

  elapsed_cpu_seconds = (double) ( time(0) - start_time );
}

// sim_t::find_player =======================================================

player_t* sim_t::find_player( const std::string& name )
{
  for( player_t* p = player_list; p; p = p -> next )
  {
    if( name == p -> name() ) return p;
  }
  return 0;
}

// sim_t::parse_option ======================================================

bool sim_t::parse_option( const std::string& name,
			  const std::string& value )
{
  option_t options[] =
  {
    { "average_dmg",                      OPT_INT8,   &( average_dmg                              ) },
    { "channel_penalty",                  OPT_FLT,    &( channel_penalty                          ) },
    { "debug",                            OPT_INT8,   &( debug                                    ) },
    { "gcd_penalty",                      OPT_FLT,    &( gcd_penalty                              ) },
    { "html_file",                        OPT_STRING, &( html_file_str                            ) },
    { "default_strength",                 OPT_INT16,  &( gear_default.attribute[ ATTR_STRENGTH  ] ) },
    { "default_agility",                  OPT_INT16,  &( gear_default.attribute[ ATTR_AGILITY   ] ) },
    { "default_stamina",                  OPT_INT16,  &( gear_default.attribute[ ATTR_STAMINA   ] ) },
    { "default_intellect",                OPT_INT16,  &( gear_default.attribute[ ATTR_INTELLECT ] ) },
    { "default_spirit",                   OPT_INT16,  &( gear_default.attribute[ ATTR_SPIRIT    ] ) },
    { "default_spell_power",              OPT_INT16,  &( gear_default.spell_power                 ) },
    { "default_attack_power",             OPT_INT16,  &( gear_default.attack_power                ) },
    { "default_expertise_rating",         OPT_INT16,  &( gear_default.expertise_rating            ) },
    { "default_armor_penetration_rating", OPT_INT16,  &( gear_default.armor_penetration_rating    ) },
    { "default_hit_rating",               OPT_INT16,  &( gear_default.hit_rating                  ) },
    { "default_crit_rating",              OPT_INT16,  &( gear_default.crit_rating                 ) },
    { "default_haste_rating",             OPT_INT16,  &( gear_default.haste_rating                ) },
    { "delta_strength",                   OPT_INT16,  &( gear_delta.attribute[ ATTR_STRENGTH  ]   ) },
    { "delta_agility",                    OPT_INT16,  &( gear_delta.attribute[ ATTR_AGILITY   ]   ) },
    { "delta_stamina",                    OPT_INT16,  &( gear_delta.attribute[ ATTR_STAMINA   ]   ) },
    { "delta_intellect",                  OPT_INT16,  &( gear_delta.attribute[ ATTR_INTELLECT ]   ) },
    { "delta_spirit",                     OPT_INT16,  &( gear_delta.attribute[ ATTR_SPIRIT    ]   ) },
    { "delta_spell_power",                OPT_INT16,  &( gear_delta.spell_power                   ) },
    { "delta_attack_power",               OPT_INT16,  &( gear_delta.attack_power                  ) },
    { "delta_expertise_rating",           OPT_INT16,  &( gear_delta.expertise_rating              ) },
    { "delta_armor_penetration_rating",   OPT_INT16,  &( gear_delta.armor_penetration_rating      ) },
    { "delta_hit_rating",                 OPT_INT16,  &( gear_delta.hit_rating                    ) },
    { "delta_crit_rating",                OPT_INT16,  &( gear_delta.crit_rating                   ) },
    { "delta_haste_rating",               OPT_INT16,  &( gear_delta.haste_rating                  ) },
    { "html",                             OPT_STRING, &( html_file_str                            ) },
    { "infinite_energy",                  OPT_INT8,   &( infinite_resource[ RESOURCE_ENERGY ]     ) },
    { "infinite_focus",                   OPT_INT8,   &( infinite_resource[ RESOURCE_FOCUS  ]     ) },
    { "infinite_health",                  OPT_INT8,   &( infinite_resource[ RESOURCE_HEALTH ]     ) },
    { "infinite_mana",                    OPT_INT8,   &( infinite_resource[ RESOURCE_MANA   ]     ) },
    { "infinite_rage",                    OPT_INT8,   &( infinite_resource[ RESOURCE_RAGE   ]     ) },
    { "infinite_runic",                   OPT_INT8,   &( infinite_resource[ RESOURCE_RUNIC  ]     ) },
    { "iterations",                       OPT_INT32,  &( iterations                               ) },
    { "lag",                              OPT_FLT,    &( lag                                      ) },
    { "merge_ignite",                     OPT_INT8,   &( merge_ignite                             ) },
    { "reaction_time",                    OPT_FLT,    &( reaction_time                            ) },
    { "regen_periodicity",                OPT_FLT,    &( regen_periodicity                        ) },
    { "log",                              OPT_INT8,   &( log                                      ) },
    { "max_time",                         OPT_FLT,    &( max_time                                 ) },
    { "threads",                          OPT_INT32,  &( threads                                  ) },
    { "patch",                            OPT_STRING, &( patch_str                                ) },
    { "pet_lag",                          OPT_FLT,    &( pet_lag                                  ) },
    { "potion_sickness",                  OPT_INT8,   &( potion_sickness                          ) },
    { "seed",                             OPT_INT32,  &( seed                                     ) },
    { "sfmt",                             OPT_INT8,   &( sfmt                                     ) },
    { "timestamp",                        OPT_INT8,   &( timestamp                                ) },
    { "wiki",                             OPT_STRING, &( wiki_file_str                            ) },
    { NULL, OPT_UNKNOWN }
  };

  if( name.empty() )
  {
    option_t::print( this, options );
    return false;
  }

  if( target  -> parse_option( name, value ) ) return true;
  if( report  -> parse_option( name, value ) ) return true;
  if( scaling -> parse_option( name, value ) ) return true;

  if( active_player && active_player -> parse_option( name, value ) ) return true;

  return option_t::parse( this, options, name, value );
}

// sim_t::print_options =====================================================

void sim_t::print_options()
{
  fprintf( output_file, "\nWorld of Warcraft Raid Simulator Options:\n" );

  fprintf( output_file, "\nSimulation Engine:\n" );
  parse_option( std::string(), std::string() );

  fprintf( output_file, "\nTarget: %s\n", target -> name() );
  target -> parse_option( std::string(), std::string() );

  for( player_t* p = player_list; p; p = p -> next )
  {
    fprintf( output_file, "\nPlayer: %s (%s)\n", p -> name(), util_t::player_type_string( p -> type ) );
    p -> parse_option( std::string(), std::string() );
  }

  fprintf( output_file, "\n" );
  fflush( output_file );
}

// ==========================================================================
// MAIN 
// ==========================================================================

int main( int argc, char** argv )
{
  sim_t sim;

  if( ! option_t::parse( &sim, argc, argv ) )
  {
    fprintf( sim.output_file, "ERROR! Incorrect option format..\n" );
    exit( 0 );
  }

  if( sim.seed == 0 ) sim.seed = (int32_t) time( NULL );
  srand( sim.seed );

  fprintf( sim.output_file, 
	   "\n"
	   "SimulationCraft for World of Warcraft build %s\n"
	   "\n"
	   "Generating baseline... (iterations=%d, max_time=%.0f, target_health=%.0f)\n",
	   sim.patch_str.c_str(), sim.iterations, sim.max_time, sim.target -> initial_health );
  fflush( sim.output_file );

  sim.execute();

  sim.scaling -> analyze();

  sim.report -> print();
  sim.report -> chart();

  if( sim.output_file != stdout ) fclose( sim.output_file );
  
  return 0;
}

// ==========================================================================
// Utility to make sure memory allocation not happening during iteration.
// ==========================================================================

#if 0
void* operator new (size_t size)
{
  if( iterating ) assert(0);
  return malloc(size); 
}

void operator delete (void *p)
{
 free(p); 
}
#endif
