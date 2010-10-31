#include <stdlib.h>
#include <string.h>
#include <util.h>
#include <ecl_sum.h>
#include <double_vector.h>
#include <time_t_vector.h>
#include <config.h>
#include <vector.h>
#include <glob.h>
#include <statistics.h>

#define DEFAULT_NUM_INTERP  50
#define SUMMARY_JOIN       ":"



typedef enum {
  S3GRAPH    = 1,
  HEADER = 2,
  PLAIN      = 3
} format_type;


typedef struct {
  ecl_sum_type             * ecl_sum;
  double_vector_type       * interp_data;
  const time_t_vector_type * interp_time;
  time_t                     start_time;
  time_t                     end_time;
} sum_case_type;


typedef struct  {
  stringlist_type * keys;
  char            * file;
  format_type       format;
} output_type;



typedef struct {
  vector_type         * data;
  time_t_vector_type  * interp_time;
  int                   num_interp;
  time_t                start_time;
  time_t                end_time;
  const ecl_sum_type  * refcase;     /* Pointer to an arbitrary ecl_sum instance in the ensemble - to have access to indexing functions. */
} ensemble_type;



#define S3GRAPH_STRING "S3GRAPH"
#define HEADER_STRING  "HEADER"
#define PLAIN_STRING   "PLAIN"



/*****************************************************************/


sum_case_type * sum_case_fread_alloc( const char * data_file , const time_t_vector_type * interp_time ) {
  sum_case_type * sum_case = util_malloc( sizeof * sum_case , __func__ );

  printf("Loading case: %s\n" , data_file );
  sum_case->ecl_sum     = ecl_sum_fread_alloc_case( data_file , SUMMARY_JOIN );
  sum_case->interp_data = double_vector_alloc(0 , 0);
  sum_case->interp_time = interp_time; 
  sum_case->start_time  = ecl_sum_get_start_time( sum_case->ecl_sum );
  sum_case->end_time    = ecl_sum_get_end_time( sum_case->ecl_sum );
  return sum_case;
}


void sum_case_free( sum_case_type * sum_case) {
  ecl_sum_free( sum_case->ecl_sum );
  double_vector_free( sum_case->interp_data );
  free( sum_case );
}


void sum_case_free__( void * sum_case) {
  sum_case_free( (sum_case_type *) sum_case);
}


/*****************************************************************/


void ensemble_add_case( ensemble_type * ensemble , const char * data_file ) {
  sum_case_type * sum_case = sum_case_fread_alloc( data_file , ensemble->interp_time );
  vector_append_owned_ref( ensemble->data , sum_case , sum_case_free__ );
  if (ensemble->start_time > 0)
    ensemble->start_time = util_time_t_min( ensemble->start_time , sum_case->start_time);
  else
    ensemble->start_time = ecl_sum_get_start_time( sum_case->ecl_sum );
  
  ensemble->end_time   = util_time_t_max( ensemble->end_time   , sum_case->end_time);
}



void ensemble_init_time_interp( ensemble_type * ensemble ) {
  int i;
  for (i = 0; i < ensemble->num_interp; i++)
    time_t_vector_append( ensemble->interp_time , ensemble->start_time + i * (ensemble->end_time - ensemble->start_time) / (ensemble->num_interp - 1));
}



void ensemble_load_from_glob( ensemble_type * ensemble , const char * pattern ) {
  glob_t pglob;
  int    i;
  glob( pattern , GLOB_NOSORT , NULL , &pglob );

  for (i=0; i < pglob.gl_pathc; i++)
    ensemble_add_case( ensemble , pglob.gl_pathv[i]);

  globfree( &pglob );
}



ensemble_type * ensemble_alloc( ) {
  ensemble_type * ensemble = util_malloc( sizeof * ensemble , __func__ );

  ensemble->num_interp  = DEFAULT_NUM_INTERP;
  ensemble->start_time  = -1;
  ensemble->end_time    = -1;
  ensemble->data        = vector_alloc_new();
  ensemble->interp_time = time_t_vector_alloc( 0 , -1 );
  
  return ensemble;
}


void ensemble_init( ensemble_type * ensemble , config_type * config) {

  /*1 : Loading ensembles and settings from the config instance */
  /*1a: Loading the eclipse summary cases. */
  {
    int i,j;
    for (i=0; i < config_get_occurences( config , "CASE_LIST"); i++) {
      const stringlist_type * case_list = config_iget_stringlist_ref( config , "CASE_LIST" , i );
      for (j=0; j < stringlist_get_size( case_list ); j++)
        ensemble_load_from_glob( ensemble , stringlist_iget( case_list , j ));
    }
  }
  {
    const sum_case_type * tmp = vector_iget_const( ensemble->data , 0 );
    ensemble->refcase = tmp->ecl_sum;
  }
  
  /*1b: Other config settings */
  if (config_item_set( config , "NUM_INTERP" ))
    ensemble->num_interp  = config_iget_as_int( config , "NUM_INTERP" , 0 , 0 );
  
  
  /*2: Remaining initialization */
  ensemble_init_time_interp( ensemble );
  if (vector_get_size( ensemble->data ) < 10 )
    util_exit("Sorry - quantiles make no bloody sense with with < 10 realizations; should have ~> 100.\n");
}




void ensemble_free( ensemble_type * ensemble ) {
  vector_free( ensemble->data );
  time_t_vector_free( ensemble->interp_time );
  free( ensemble );
}

/*****************************************************************/
 
static output_type * output_alloc( const char * file , const char * format_string) {
  output_type * output = util_malloc( sizeof * output , __func__);
  output->keys = stringlist_alloc_new();
  output->file = util_alloc_string_copy( file );
  {
    format_type  format;

    if ( util_string_equal(format_string , S3GRAPH_STRING))
      format = S3GRAPH;
    else if ( util_string_equal( format_string , HEADER_STRING))
      format = HEADER;
    else if ( util_string_equal( format_string , PLAIN_STRING) )
      format = PLAIN;
    else
      util_abort("%s: unrecognized format string:%s \n",__func__ , format_string);
    
    output->format = format;
  }
  
  return output;
}



static void output_free( output_type * output ) {
  stringlist_free( output->keys );
  free( output->file );
  free( output );
}

static void output_free__( void * arg) {
  output_free( (output_type *) arg);
}
  
static void output_add_key( output_type * output , const char * key) {
  stringlist_append_copy( output->keys , key );
}


/*****************************************************************/

/**
   Each output line should be of the format:

   OUTPUT  output_file key.q    key.q    key.q    key.q    ...
*/

void output_table_init( hash_type * output_table , const config_type * config ) {
  int i,j;
  for (i=0; i < config_get_occurences( config , "OUTPUT" ); i++) {
    const stringlist_type * tokens = config_iget_stringlist_ref( config , "OUTPUT" , i);
    const char * file              = stringlist_iget( tokens , 0 );
    const char * format_string     = stringlist_iget( tokens , 1 );
    output_type * output           = output_alloc( file , format_string );
    
    /* Alle the keys are just added - without any check. */
    for (j = 2; j < stringlist_get_size( tokens ); j++)
      output_add_key( output , stringlist_iget( tokens , j));
    
    hash_insert_hash_owned_ref( output_table , file , output , output_free__ );
  }
}


/**
   Will print the string variable @var and the numerical variable @q
   padded to a total width of @w:

        'var:0.001     '
*/

static void print_var( FILE * stream , const char * var , double q , int w) {
  int qwidth       = 5;
  const char * fmt = "%s:%4.2f";
  
  for (int i = 0; i < (w - strlen( var )) - qwidth; i++)
    fprintf(stream , " ");      
  fprintf(stream , fmt , var , q);
  fprintf(stream , " ");  /* Alwasy include one extra space to insure againt coaleching strings. */
  
}


/* 
   I don't understand the rules of the game when it comes to parsing
   the header section of the S3Graph files, it can actually seem like
   some 'fixed-length' magic is going on.

   An ECLIPSE summary variable is generally characterized by three
   variable values from SMSPEC vectors; the three vectors are
   KEYWORDS, WGNAMES and NUMS.

   The main variable is the KEYWORDS variable which says what type of
   variable it is. Examples of KEYWORDS variable values are 'WWCT',
   'WOPR' and 'GWPT'. To make the variable unique we then need
   additional information from one, or both of WGNAMES and NUMS. For
   instance for a well variable or group variable we will need the
   well name from WGNAMES and for a block property we will need the
   block number (as i + j*nx + k*nx*ny) from the NUMS vector. 

   When writing the S3Graph header I don't understand how to enter the
   different parts of header information. The current implementation,
   which seems to work reasonably well[*] , does the following:

     1. Write a line like this:

             TIME    DATE        KEYWORD1:xxx    KEYWORD2:xxx     KEYWORD3:xxxx
          
        Here KEYWORD is an eclipse variable memnonic from the KEYWORDS
        array, i.e. FOPT or WWCT. The :xxx part is the quantile we are
        looking at, i.e. 0.10 or 0.90. It seems adding the quantile
        does not confuse S3Graph.

     2. Write a line with units:

             TIME    DATA        KEYWORD1:xxx    KEYWORD2:xxx     KEYWORD3:xxxx 
             DAYS                UNIT1           UNIT2            UNIT2             <---- New line


     3. Write a line with keyword qualifiers, i.e. extra information:

             TIME    DATA        WOPR:xxx        FOPT:xxxx        BPR
             DAYS                UNIT1           UNIT2            UNIT2             
                                 OP1                              1000              <---- New line 
 
        Now - the totally confusing part is that it is not clear what
        S3Graph expects on this third line, in the case of well/group
        variables it is a well/group name from the WGNAMES array,
        whereas for e.g. a region or block varaiable it wants an
        element from the NUMS array, and for e.g. a field variable it
        wants nothing extra[**]. When it comes to variables which need
        both NUMS and WGNAMES to become unique (e.g completion
        variables) it is not clear how - if at all possible - to
        support it. In the current implementation a string
        concatenation of WGNAMES and NUMS is used.



   [*] : I do not really understand why it seems to work.

   [**]: It seemingly manages to pick out the right qualifier - how
         that works I don't know; but I try be reeally nazi with the
         formatting.
*/




void output_save_S3Graph( const char * file , ensemble_type * ensemble , const double ** data , const stringlist_type * ecl_keys, const double_vector_type * quantiles) {
  FILE * stream = util_mkdir_fopen( file , "w"); 
  int          field_width  = 24;
  const char * unit_fmt     = "%24s ";
  const char * num_fmt      = "%24d ";
  const char * float_fmt    = "%24.5f ";
  const char * days_fmt     = "%10.2f ";
  const char * date_fmt     = "%02d-%02d-%04d ";
  const char * time_header  = "      DATE       TIME ";
  const char * time_unit    = "                 DAYS ";
  const char * time_blank   = "                      ";
  const int    data_columns = stringlist_get_size( ecl_keys );
  const int    data_rows    = time_t_vector_size( ensemble->interp_time );
  int row_nr,column_nr;
    
  {
    char       * origin; 
    util_alloc_file_components( file , NULL ,&origin , NULL);
    fprintf(stream , "ORIGIN %s\n", origin );
    free( origin );
  }
  
  /* 1: Writing first header line with variables. */
  fprintf(stream , time_header );
  for (column_nr = 0; column_nr < data_columns; column_nr++) {
    const char * ecl_key = stringlist_iget( ecl_keys , column_nr );
    double quantile      = double_vector_iget( quantiles , column_nr );
    print_var( stream , ecl_sum_get_keyword( ensemble->refcase , ecl_key ) , quantile , field_width);
  }
  fprintf(stream , "\n");
  
  
  /* 2: Writing second header line with units. */
  fprintf(stream , time_unit );
  for (column_nr = 0; column_nr < data_columns; column_nr++) {
    const char * ecl_key = stringlist_iget( ecl_keys , column_nr );
    double quantile      = double_vector_iget( quantiles , column_nr );
    fprintf(stream , unit_fmt , ecl_sum_get_unit( ensemble->refcase , ecl_key ) , quantile);
  }
  fprintf(stream , "\n");
  
  
  /*3: Writing third header line with WGNAMES / NUMS - extra information - 
       breaks completely down with LGR information. */
  fprintf(stream , time_blank );
  {
    for (column_nr = 0; column_nr < data_columns; column_nr++) {
      const char * ecl_key         = stringlist_iget( ecl_keys , column_nr );
      const char * wgname          = ecl_sum_get_wgname( ensemble->refcase , ecl_key ); 
      int          num             = ecl_sum_get_num( ensemble->refcase , ecl_key );
      ecl_smspec_var_type var_type = ecl_sum_get_var_type( ensemble->refcase , ecl_key);
      bool need_num                = ecl_smspec_needs_num(   var_type );
      bool need_wgname             = ecl_smspec_needs_wgname(   var_type );      
      
      if (need_num && need_wgname) {
        /** Do not know have to include both - will just create a
            mangled name as a combination. */
        char * wgname_num = util_alloc_sprintf("%s:%d" , wgname , num);
        fprintf(stream , unit_fmt , wgname_num);
        free( wgname_num );
      } else if (need_num)
        fprintf(stream , num_fmt , num);
      else if (need_wgname)
        fprintf(stream , unit_fmt , wgname);
      else
        fprintf(stream , unit_fmt , " ");
    }
    fprintf(stream , "\n");
  }
  

  /*4: Writing the actual data. */
  for (row_nr = 0; row_nr < data_rows; row_nr++) {
    time_t interp_time = time_t_vector_iget( ensemble->interp_time , row_nr);
    {
      int mday,month,year;
      util_set_datetime_values(interp_time , NULL , NULL , NULL , &mday , &month , &year);
      fprintf(stream , date_fmt , mday , month , year);
    }
    fprintf(stream , days_fmt , 1.0*(interp_time - ensemble->start_time) / 86400);
      
    for (column_nr = 0; column_nr < data_columns; column_nr++) {
      fprintf(stream , float_fmt , data[row_nr][column_nr]);
    }
    fprintf( stream , "\n");
  }
}



void output_save_plain__( const char * file , ensemble_type * ensemble , const double ** data , const stringlist_type * ecl_keys, const double_vector_type * quantiles , bool add_header) {
  FILE * stream = util_mkdir_fopen( file , "w"); 
  int          field_width  = 24;
  const char * key_fmt      = " %18s:%4.2f ";
  const char * time_header  = "--    DAYS      DATE    ";
  const char * time_dash    = "------------------------";
  const char * key_dash     = "-------------------------";
  const char * float_fmt    = "%24.5f ";
  const char * days_fmt     = "%10.2f ";
  const char * date_fmt     = "  %02d/%02d/%04d ";
  const int    data_columns = stringlist_get_size( ecl_keys );
  const int    data_rows    = time_t_vector_size( ensemble->interp_time );
  int row_nr,column_nr;

  if (add_header) {
    fprintf( stream ,time_header);
    for (int i=0; i < stringlist_get_size( ecl_keys ); i++) 
      fprintf( stream , key_fmt , stringlist_iget( ecl_keys , i) , double_vector_iget( quantiles , i ));
    fprintf(stream , "\n");

    fprintf( stream , time_dash );
    for (int i=0; i < stringlist_get_size( ecl_keys ); i++) 
      fprintf(stream , key_dash );
    fprintf(stream , "\n");
  }

  /*4: Writing the actual data. */
  for (row_nr = 0; row_nr < data_rows; row_nr++) {
    time_t interp_time = time_t_vector_iget( ensemble->interp_time , row_nr);
    fprintf(stream , days_fmt , 1.0*(interp_time - ensemble->start_time) / 86400);
    {
      int mday,month,year;
      util_set_datetime_values(interp_time , NULL , NULL , NULL , &mday , &month , &year);
      fprintf(stream , date_fmt , mday , month , year);
    }
      
    for (column_nr = 0; column_nr < data_columns; column_nr++) {
      fprintf(stream , float_fmt , data[row_nr][column_nr]);
    }
    fprintf( stream , "\n");
  }
}




void output_save( const char * file , ensemble_type * ensemble , const double ** data , const stringlist_type * ecl_keys , const double_vector_type * quantiles , format_type format) {
  switch( format ) {
  case(S3GRAPH):
    output_save_S3Graph( file , ensemble , data , ecl_keys ,  quantiles);
    break;
  case(PLAIN):
    output_save_plain__( file , ensemble , data , ecl_keys , quantiles , false);
    break;
  case(HEADER):
    output_save_plain__( file , ensemble , data , ecl_keys , quantiles , true);
    break;
  default:
    util_exit("Sorry: output_format:%d not supported \n", format );
  }
}
      




void output_run_line( const output_type * output , ensemble_type * ensemble) {
  
  const int    data_columns = stringlist_get_size( output->keys );
  const int    data_rows    = time_t_vector_size( ensemble->interp_time );
  double     ** data;
  int row_nr, column_nr;

  stringlist_type * sum_keys     = stringlist_alloc_new();
  double_vector_type * quantiles = double_vector_alloc(0,0);
  
  data = util_malloc( data_rows * sizeof * data , __func__);
  /*
    time-direction, i.e. the row index is the first index and the
    column number (i.e. the different keys) is the second index. 
  */
  for (row_nr=0; row_nr < data_rows; row_nr++)
    data[row_nr] = util_malloc( sizeof * data[row_nr] * data_columns, __func__);
  
  printf("Creating output file: %s \n",output->file );

  /* Going through the keys. */
  for (column_nr = 0; column_nr < stringlist_get_size( output->keys ); column_nr++) {
    const char * key = stringlist_iget( output->keys , column_nr );
    char * sum_key;
    double quantile;
    {
      int tokens;
      char ** tmp;
      util_split_string( key , SUMMARY_JOIN , &tokens , &tmp);
      if (tokens == 1)
        util_exit("Hmmm - the key:%s is malformed - must be of the form SUMMARY_KEY:QUANTILE.\n",key);
      
      sum_key = util_alloc_joined_string( (const char **) tmp , tokens - 1 , SUMMARY_JOIN);
      if (!util_sscanf_double( tmp[tokens - 1] , &quantile))
        util_exit("Hmmmm - failed to interpret:%s as a quantile - must be a number [0,1).\n",tmp[tokens-1]);
      
      util_free_stringlist( tmp, tokens );
    }
    double_vector_append( quantiles , quantile );
    stringlist_append_owned_ref( sum_keys , sum_key );
  }

  
  
  /* The main loop - outer loop is running over time. */
  {
    hash_type * interp_data_cache = hash_alloc();

    for (row_nr = 0; row_nr < data_rows; row_nr++) {
      time_t interp_time = time_t_vector_iget( ensemble->interp_time , row_nr);
      for (column_nr = 0; column_nr < stringlist_get_size( sum_keys ); column_nr++) {
        const char * sum_key = stringlist_iget( sum_keys , column_nr);
        double quantile      = double_vector_iget( quantiles , column_nr);
        double_vector_type * interp_data;

        /* Check if we have the vector in the cache table - if not create it. */
        if (!hash_has_key( interp_data_cache , sum_key)) {
          interp_data = double_vector_alloc(0 , 0);
          hash_insert_hash_owned_ref( interp_data_cache , sum_key , interp_data , double_vector_free__);
        }
        interp_data = hash_get( interp_data_cache , sum_key );

        /* Check if the vector has data - if not initialize it. */
        if (double_vector_size( interp_data ) == 0) {
          /* Fill up the interpolated vector */
          for (int iens = 0; iens < vector_get_size( ensemble->data ); iens++) {
            const sum_case_type * sum_case = vector_iget_const( ensemble->data , iens );
            if ((interp_time >= sum_case->start_time) && (interp_time <= sum_case->end_time))    /* We allow the different simulations to have differing length */
              double_vector_append( interp_data , ecl_sum_get_general_var_from_sim_time( sum_case->ecl_sum , interp_time , sum_key)) ;
          }
        }
        data[row_nr][column_nr] = statistics_empirical_quantile( interp_data , quantile );
      }
      hash_apply( interp_data_cache , double_vector_reset__ );
    }
    hash_free( interp_data_cache );
  }
  
  output_save( output->file , ensemble , (const double **) data , sum_keys , quantiles , output->format );
  stringlist_free( sum_keys );
  double_vector_free( quantiles );
  for (row_nr=0; row_nr < data_rows; row_nr++)
    free( data[row_nr] );
  free( data );
}



void output_table_run( hash_type * output_table , ensemble_type * ensemble ) {
  hash_iter_type * iter = hash_iter_alloc( output_table);

  while (!hash_iter_is_complete( iter )) {
    const char * output_file     = hash_iter_get_next_key( iter );
    const output_type * output   = hash_get( output_table , output_file );
    output_run_line( output, ensemble );
  }
}




/*****************************************************************/

void config_init( config_type * config ) {


  config_add_item( config , "CASE_LIST"      , true , true );
  config_add_key_value( config , "NUM_INTERP" , false , CONFIG_INT);
  
  {
    config_item_type * item;
    item = config_add_item( config , "OUTPUT" , true , true );
    config_item_set_argc_minmax( item , 2 , -1 , NULL );
    /*
      This does not work with open arg max:

      config_item_set_indexed_selection_set( item , 1 , 3 , (const char *[3]) { S3GRAPH_STRING , HEADER_STRING , PLAIN_STRING });
    */
  }
}


/*****************************************************************/

void usage() {
  fprintf(stderr, "\nUse:\n\n    ecl_quantil config_file\n\n");
  
  printf("Help\n");
  printf("----\n");
  printf("\n");
  printf("The ecl_quantile program will load an ensemble of ECLIPSE summary\n");
  printf("files, it can then output quantiles of summary vectors over the time\n");
  printf("span of the simulation. The program is based on a simple configuration\n");
  printf("file. The configuration file only has three keywords:\n");
  printf("\n");
  printf("\n");
  printf("   CASE_LIST   simulation*X/run*X/CASE*.DATA\n");
  printf("   CASE_LIST   extra_simulation.DATA    even/more/simulations*GG/run*.DATA\n");
  printf("\n");
  printf("\n");
  printf("   OUTPUT      FILE1   S3GRAPH WWCT:OP_1:0.10  WWCT:OP_1:0.50   WOPR:OP_3\n");
  printf("   OUTPUT      FILE2   PLAIN   FOPT:0.10  FOPT:0.50  FOPT:0.90  GOPT:0.10  GOPT:0.50  GOPT:0.90   FWPT:0.10  FWPT:0.50  FWPT:0.90\n");
  printf("   NUM_INTERP  100\n");
  printf("\n");
  printf("\n");
  printf("CASE_LIST: This keyword is used to give the path to ECLIPSE data files\n");
  printf("  corresponding to summaries which you want to load, observe that the\n");
  printf("  argument given to the CASE_LIST keyword can contain unix-style\n");
  printf("  wildcards like '*'. You can point to several simulation cases with\n");
  printf("  one CASE_LIST keyword. In addition you can several CASE_LIST\n");
  printf("  keywords.\n");
  printf("\n");
  printf("\n");
  printf("OUTPUT: This keyword is used to denote what output you want from the\n");
  printf("  program. The first argument to the OUTPUT keyword is the name output\n");
  printf("  file you want to produce, in the example above we will create two\n");
  printf("  output files (FILE1 and FILE2 respectively). The second argument is \n");
  printf("  the wanted type of the output file, the three types currently supported\n");
  printf("  are: \n\n");
  printf("     S3GRAPH: S3GRAPH user format - at least quite close...\n");
  printf("     PLAIN: Columns of data without any header information\n");
  printf("     HEADER: Like plain, but with a header at the top\n\n");
  printf("  The remaining arguments on the output line corresponds to the \n");
  printf("  summary vector & quantile you are interested in. Each of these values\n");
  printf("  is a \":\" separated string consting of:\n");
  printf("  \n");
  printf("     VAR: The ECLIPSE summary variable we are interested in, (nearly)\n");
  printf("          all variables found in the summary file are available,\n");
  printf("          e.g. RPR, WWCT or GOPT.\n");
  printf("\n");
  printf("     WG?: This is extra information added to the variable to make it\n");
  printf("          unique, e.g. the name of a well or group for rate variables\n");
  printf("          and the region number for a region. Not all variables, in\n");
  printf("          particalar the Fxxx rates, have this string.\n");
  printf("\n");
  printf("     Q: The quantile we are interested in, e.g 0.10 to get the P10\n");
  printf("        quantile and 0.90 to get the P90 quantile.\n");
  printf("\n");
  printf("  Examples are:\n");
  printf("\n");
  printf("     WWCT:OPX:0.75:    The P75 quantile of the watercut in well OPX.\n");
  printf("     BPR:10,10,5:0.50: The P50 quantile of the Block Pressure in block 10,10,5\n");
  printf("     FOPT:0.90:        The P90 quantile of the field oil production total.\n");
  printf("\n");
  printf("\n");
  printf("NUM_INTERP: Before the program can calculate quantiles it must\n");
  printf("  interpolate all the simulated data down on the same time axis. This\n");
  printf("  keyword regulates how many points should be used when interpolating\n");
  printf("  the time axis; the default is 50 which is probably quite OK. Observe\n");
  printf("  that for rate variable the program will not do linear interpolation\n");
  printf("  between ECLIPSE report steps, the might therefore look a bit jagged\n");
  printf("  if NUM_INTERP is set to high. This keyword is optional.\n");
  exit(0);
}



int main( int argc , char ** argv ) {
  if (argc != 2)
    usage();
  else {
    hash_type     * output_table   = hash_alloc();
    ensemble_type * ensemble       = ensemble_alloc();
    {
      config_type   * config   = config_alloc( );
      config_init( config );
      config_parse( config , argv[1] , "--" , NULL , NULL , false , true );
    
      ensemble_init( ensemble , config );
      output_table_init( output_table , config);
      config_free( config );
    } 
    output_table_run( output_table , ensemble );
    ensemble_free( ensemble );
    hash_free( output_table );
  }
}
