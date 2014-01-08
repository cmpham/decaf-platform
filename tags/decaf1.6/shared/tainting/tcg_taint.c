#include "qemu-common.h"

#ifdef CONFIG_TCG_TAINT

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>

#include "tcg.h"
#include "tainting/tcg_taint.h"

#include "tainting/taint_memory.h"
#include "config-target.h"

#include "helper.h" // Taint helper functions, plus I386 IN/OUT helpers
#include "tcg_taint_branch.h"

//#define BLOCK_SKIP_GREATER_EQUAL 68
//#define BLOCK_SKIP_LESS_EQUAL 66
//#define ALLOW_BSLE 1
//#define ALLOW_BSGE 1

/* Target-specific metadata buffers are extern'd here so that the taint
   IR insertions can update them. */
#ifdef CONFIG_TCG_TAINT
#if defined(TARGET_I386)
extern uint8_t gen_opc_cc_op[OPC_BUF_SIZE];
#elif defined(TARGET_ARM)
extern uint32_t gen_opc_condexec_bits[OPC_BUF_SIZE];
#endif /* TARGET_I386 */
#endif /* CONFIG_TCG_TAINT */

uint16_t *gen_old_opc_ptr;
TCGArg *gen_old_opparam_ptr;

uint32_t block_count = 0; // AWH - Debugging

// Typedef the CPU state struct to make tempidx ld/st cleaner
#if defined(TARGET_I386)
typedef CPUX86State OurCPUState;
#elif defined(TARGET_ARM)
typedef CPUARMState OurCPUState;
#elif defined(TARGET_MIPS)
typedef CPUMIPSState OurCPUState;
#endif /* TARGET_I386/ARM */

// AWH - In development
//#define TCG_LOGGING_TAINT 1
//#define TCG_TAINT_BRANCHING 1
#define USE_TCG_OPTIMIZATIONS 1
//#define LOG_POINTER
#define LOG_TAINTED_EIP
// AWH - Change these to change taint/pointer rules
#define TAINT_EXPENSIVE_ADDSUB 1
#define TCG_BITWISE_TAINT 1
//#define TAINT_NEW_POINTER 1

#if defined(LOG_POINTER) || defined(LOG_TAINTED_EIP)
#define MAX_TAINT_LOG_TEMPS 10
static TCGArg helper_arg_array[MAX_TAINT_LOG_TEMPS];
#define MAX_TAINT_LOG_TEMPS 10
static TCGv taint_log_temps[MAX_TAINT_LOG_TEMPS];
static inline void set_con_i32(int index, TCGv arg)
{

	  tcg_gen_mov_i32(taint_log_temps[index], arg);
	  helper_arg_array[index] = taint_log_temps[index];
}

#endif

// Exposed externs
TCGv shadow_arg[TCG_MAX_TEMPS];
TCGv tempidx, tempidx2;

// Extern in translate.c
extern TCGv_ptr cpu_env;

#ifdef TCG_TAINT_BRANCHING

static int local_no_taint_label = 0;
static int local_complete_label = 0;

#if 0 // AWH - In development
/* Basic Block node type for SSA transformation process */
typedef struct BB_node {
  /* First opcode/param for this BB node */
  unsigned int begin_opcode_offset; /* Opcode index where this BB starts */
  unsigned int begin_opparam_offset; /* Opparam index where this BB starts */
  /* Size of this node in opcodes/params */
  unsigned int num_opcodes;
  unsigned int num_params;
  /* Buffers to hold the opcodes/params */
  uint16_t opc_buffer[OPC_BUF_SIZE];
  TCGArg opparam_buffer[OPPARAM_BUF_SIZE];
  /* IDs of next node(s) (-1 if none) */
  signed int BB_fallthrough_index;
  signed int BB_branch_index;
  /* When we first construct the graph, we'll have references to labels that
     we have not come across yet.  If this node has a branch to a label, that
     label number is recorded here.  Later, once we've identified all of the
     nodes, we'll sweep through each and resolve BB_branch_index to the
     proper BB_node.  This is -1 if there is no label being branched to. */
  signed int waiting_for_label;
  /* Label for this BB (-1 if none) */
  signed int BB_label;
} BB_node_t;

/* This is an array of the CFG nodes, so we can just traverse them without
   having to follow links (for resolving labels). */
#define MAX_BB_NODES 1024
static BB_node_t *BB_node_list[MAX_BB_NODES];

/* Create a BB node */
static inline BB_node_t *createBBNode(void) {
  BB_node_t *new_node = (BB_node_t *)calloc(1, sizeof(BB_node_t));
  new_node->BB_fallthrough_index = -1;
  new_node->BB_branch_index = -1;
  new_node->waiting_for_label = -1;
  new_node->BB_label = -1;
}

/* Sweep through the current TB and construct the CFG */
static void constructCFG(int nb_opc, uint16_t *opc_buf, uint32_t *opparam_buf)
{
  int i = 0, x = 0;
  uint16_t opc;
  int opparam_index = 0;
  int opc_index = 0;
  int current_BB_node_index = 0;
  BB_node_t *current_node = NULL;
  

  /* Reset the CFG to empty */
  for (i = 0; i < MAX_BB_NODES; i++)
  { 
    if (BB_node_list[i]) 
    {  
      free(BB_node_list[i]);
      BB_node_list[i] = NULL;
    }
  }

  /* Create the first node */
  current_node = createBBNode();
  BB_node_list[current_BB_node_index++] = current_node;

  /* Traverse the opcodes of the TB */  
  for (i = 0; i < nb_opc; i++) {
    opc = opc_buf[i];

    /* Determine the number and type of arguments for the opcode */
    if (opc == INDEX_op_call) {
      TCGArg arg = opparam_buf[opparam_index];
      nb_oargs = arg >> 16;
      nb_iargs = arg & 0xffff;
      nb_cargs = tcg_op_defs[opc].nb_cargs;
      nb_args = nb_oargs + nb_iargs + nb_cargs + 1;
    } else if (opc == INDEX_op_nopn) {
      nb_args = nb_cargs = opparam_buf[opparam_index];
      nb_oargs = nb_iargs = 0;
    } else {
      nb_args = tcg_op_defs[opc].nb_args;
      nb_oargs = tcg_op_defs[opc].nb_oargs;
      nb_iargs = tcg_op_defs[opc].nb_iargs;
      nb_cargs = tcg_op_defs[opc].nb_cargs;

      /* Start the logic that places ops and opparms into BBs */
      switch (opc) {

        /* These are the ops that conditionally END a BB with a label */
        case INDEX_op_brcond_i32:
        case INDEX_op_local_brcond_i32:
        case INDEX_op_brcond_i64:
        case INDEX_op_local_brcond_i64
#if (TCG_TARGET_REG_BITS == 32)
        case INDEX_op_brcond2_i32:
        case INDEX_op_local_brcond2_i32:
#endif /* TCG_TARGET_REG_BITS */
          /* Set up fallthrough link */
          current_node->BB_fallthrough_index = current_BB_node_index;
          /* Set the label to resolve later */
          current_node->waiting_for_label = 
          /* Add op and opparms to current BB */
          current_node->opc_buffer[current_node->num_opcodes++] = opc;
          for (x = 0; x < nb_args; x++)
            current_node->opparam_buffer[current_node->num_opparams++] = opparam_buf[opparam_index++];
          /* Create a new node */
          current_node = createBBNode();
          BB_node_list[current_BB_node_index++] = current_node;
          break;

        /* These are the ops that unconditionally END a BB with a label */
        case INDEX_op_br:
        case INDEX_op_local_br:
          current_node->waiting_for_label =
        /* These are the ops that unconditionally END a BB without a label */
        case INDEX_op_goto_tb:
        case INDEX_op_exit_tb:
        case INDEX_op_jmp:
          /* Add op and opparams to current BB */
          current_node->opc_buffer[current_node->num_opcodes++] = opc;
          for (x = 0; x < nb_args; x++)
            current_node->opparam_buffer[current_node->num_opparams++] = opparam_buf[opparam_index++];
          /* Create a new node */
          current_node = createBBNode();
          BB_node_list[current_BB_node_index++] = current_node;        
          break;

        /* Mark the BB with its start label*/
        case INDEX_op_set_label:
          current_node->BB_label = opparam_buf[opparam_index];
        /* The default case (just store the op and opparms in the BB */
        default: 
          /* Add op and opparams to current BB */
          current_node->opc_buffer[current_node->num_opcodes++] = opc;
          for (x = 0; x < nb_args; x++)
            current_node->opparam_buffer[current_node->num_opparams++] = opparam_buf[opparam_index++];
          break;
      }
    }    
  }
}
#endif // AWH - In development

static int cc_in_use = 0;
static int global_in_use = 0;
static int cond_used = 0;

#if defined(TARGET_I386)
// AWH - Defined in target-i386/translate.c
extern TCGv cpu_cc_src, cpu_cc_dst, cpu_cc_tmp;
extern TCGv_i32 cpu_cc_op;
#endif /* TARGET_I386 */

static inline int check_global_arg(TCGv arg)
{
  return (arg < tcg_ctx.nb_globals);
}

static inline int check_cc_arg(TCGv arg)
{
#if defined(TARGET_I386)
  if((arg == cpu_cc_src) ||
     (arg == cpu_cc_dst) ||
     (arg == cpu_cc_tmp) ||
     (arg == cpu_cc_op))
    return 1;
  else
    return 0;
#else
#error Implement this
#endif /* TARGET_I386 */
}

/* This holds our metadata for each temporary register to tell
  whether it has been initialized (1) or is still uninitialized
  (0).  If a temporary is used as an input to an IR, it is 
  considered to be initialized.  All global registers are 
  considered to be initialized at all times. */
static uint8_t gen_opc_init_metadata[METADATA_SIZE];
#endif /* TCG_TAINT_BRANCHING */

/* This holds out metadata for each opcode to tell whether to 
  override the liveness optimizer pass (in tcg/tcg.c) for that
  opcode.  Even if an opcode looks like it should be removed 
  according to the rules, it won't be removed if this is set (1)
  for that opcode.  If it is not set (0), the opcode is not immune
  from the optimization logic and can be removed if needed.  This 
  is necessary to avoid having it optimize out taint branching 
  opcode paths. This is shared out to tcg.c via an extern. */
uint8_t gen_opc_opt_immune_metadata[METADATA_SIZE];

#ifdef TCG_LOGGING_TAINT
/* These are the number of temps that are created for the purpose of
  passing concrete values and registers into the taint logging helpers. */
#define MAX_TAINT_LOG_TEMPS 12
static TCGv taint_log_temps[MAX_TAINT_LOG_TEMPS];

/* Used for building up lists of args for the logging helper funcs */
static TCGArg helper_arg_array[MAX_TAINT_LOG_TEMPS];

static inline void set_concrete_i32(int index, TCGv arg)
{
  tcg_gen_mov_i32(taint_log_temps[index], arg);
  helper_arg_array[index] = taint_log_temps[index];
}

static inline void set_concrete_LHS_i32(int index, TCGv arg)
{
  //if (tcg_ctx.temps[arg].val_type == TEMP_VAL_DEAD)
  if (arg >= tcg_ctx.nb_globals)
    tcg_gen_movi_i32(taint_log_temps[index], 0);
  else
    tcg_gen_mov_i32(taint_log_temps[index], arg);
  helper_arg_array[index] = taint_log_temps[index];
}

static inline void set_arg_i32(int index, TCGv arg)
{
  tcg_gen_movi_i32(taint_log_temps[index], arg);
  helper_arg_array[index] = taint_log_temps[index];
}
#endif /* TCG_LOGGING_TAINT */

/*static*/ TCGv find_shadow_arg(TCGv arg)
{
  if (arg < tcg_ctx.nb_globals)
    return shadow_arg[arg];

  /* Check if this temp is allocated in the context */
  if (!tcg_ctx.temps[arg].temp_allocated)
    return 0;

  if (!tcg_ctx.temps[shadow_arg[arg]].temp_allocated) {
    if (tcg_ctx.temps[arg].temp_local)
#if TCG_TARGET_REG_BITS == 32 
      shadow_arg[arg] = tcg_temp_local_new_i32();
    else
      shadow_arg[arg] = tcg_temp_new_i32();
#else
      shadow_arg[arg] = tcg_temp_local_new_i64();
    else
      shadow_arg[arg] = tcg_temp_new_i64();
#endif
    // CLEAR TAINT ON CREATION
    tcg_ctx.temps[shadow_arg[arg]].val = 0;
  }

  return shadow_arg[arg];
}

void clean_shadow_arg(void)
{
  bzero(&shadow_arg[tcg_ctx.nb_globals], sizeof(shadow_arg[0]) * (TCG_MAX_TEMPS - tcg_ctx.nb_globals));
}

/* AWH - Dummy generic taint rule to make sure we have the proper
   shadow taint temps in place */
static void DUMMY_TAINT(int nb_oargs, int nb_args)
{
  TCGv arg0, orig0;

  int i = 0;
  for (i = 0; i < nb_oargs; i++)
  {
    arg0 = find_shadow_arg(gen_opparam_ptr[(-1 * nb_args) + i]);
    orig0 = gen_opparam_ptr[(-1 * nb_args) + i];
    if (arg0) {
#if TCG_TARGET_REG_BITS == 32
      tcg_gen_movi_i32(arg0, 0);
#else
      tcg_gen_movi_i64(arg0, 0);
#endif
    }
#ifdef TCG_TAINT_BRANCHING
    orig0 = gen_opparam_ptr[(-1 * nb_args) + i];
    if (orig0) {
      gen_opc_init_metadata[orig0] = 1;
    }
#endif /* TCG_TAINT_BRANCHING */
  }
}

#ifdef USE_TCG_OPTIMIZATIONS
/* This holds our metadata for each of the original opcodes to
  tell whether it will be elimintaed in liveness checks (0) or
  will still remain alive (1) and must be instrumented. */
static uint8_t gen_old_liveness_metadata[OPC_BUF_SIZE];
static void build_liveness_metadata(TCGContext *s);
#endif /* USE_TCG_OPTIMIZATIONS */

static inline int gen_taintcheck_insn(int search_pc)
{
#ifdef CONFIG_TCG_TAINT
  /* Opcode and parameter buffers */
  static uint16_t gen_old_opc_buf[OPC_BUF_SIZE];
  static TCGArg gen_old_opparam_buf[OPPARAM_BUF_SIZE];
  /* Metadata buffers for "search_pc" TBs */
  static target_ulong gen_old_opc_pc[OPC_BUF_SIZE];
  static uint8_t gen_old_opc_instr_start[OPC_BUF_SIZE];
  static uint16_t gen_old_opc_icount[OPC_BUF_SIZE];
#if defined(TARGET_I386)
  static uint8_t gen_old_opc_cc_op[OPC_BUF_SIZE];
  /* For INB, INW, INL helper functions */
  int in_helper_func = 0;
  /* For OUTB, OUTW, OUTL helper functions */
  int out_helper_func = 0;
#elif defined(TARGET_ARM)
  static uint32_t gen_old_opc_condexec_bits[OPC_BUF_SIZE];
#endif /* TARGET check */
  int metabuffer_offset = 0;

  int nb_opc = gen_opc_ptr - gen_old_opc_ptr;
  int return_lj = -1;

  int nb_args=0;
  int opc_index=0, opparam_index=0;
  int i=0, x=0;
  uint16_t opc=0;
  int nb_oargs=0, nb_iargs=0, nb_cargs=0;
  TCGv arg0, arg1, arg2, arg3, arg4, arg5, arg6;
  TCGv t0, t1, t2, t3, t4, t_zero;
  TCGv orig0, orig1, orig2, orig3, orig4, orig5;

  /* Copy all of the existing ops/parms into a new buffer to back them up. */
  memcpy(gen_old_opc_buf, gen_old_opc_ptr, sizeof(uint16_t)*(nb_opc));
  memcpy(gen_old_opparam_buf, gen_old_opparam_ptr, sizeof(TCGArg)* (gen_opparam_ptr - gen_old_opparam_ptr));

  /* If we're inserting taint IR into a searchable TB, copy all of the
     existing metadata for the TB into a new buffer to back them up. */
  if (search_pc) {
    /* Figure out where we're starting in the metabuffers */
    metabuffer_offset = gen_old_opc_ptr - gen_opc_buf;
   
    /* Make our backup copies of the metadata buffers */ 
    memcpy(gen_old_opc_pc, (gen_opc_pc + metabuffer_offset), sizeof(target_ulong)*(nb_opc));
    memcpy(gen_old_opc_instr_start, (gen_opc_instr_start + metabuffer_offset), sizeof(uint8_t)*(nb_opc));
    memcpy(gen_old_opc_icount, (gen_opc_icount + metabuffer_offset), sizeof(uint16_t)*(nb_opc));
#if defined(TARGET_I386)
    memcpy(gen_old_opc_cc_op, (gen_opc_cc_op + metabuffer_offset), sizeof(uint8_t)*(nb_opc));
#elif defined(TARGET_ARM)
    memcpy(gen_old_opc_condexec_bits, (gen_opc_condexec_bits + metabuffer_offset), sizeof(uint32_t)*(nb_opc));
#endif /* TARGET check */

    memset(gen_opc_instr_start + metabuffer_offset, 0, sizeof(uint8_t) * (OPC_BUF_SIZE - metabuffer_offset)); 
  }

  /* Reset the ops/parms buffers */
  gen_opc_ptr = gen_old_opc_ptr;
  gen_opparam_ptr = gen_old_opparam_ptr;

#if defined(TCG_LOGGING_TAINT) || defined(LOG_POINTER) || defined(LOG_TAINTED_EIP)
  /* Allocate our temps for logging taint */
  for (i=0; i < MAX_TAINT_LOG_TEMPS; i++)
#if TCG_TARGET_REG_BITS == 32
    taint_log_temps[i] = tcg_temp_new_i32();
#else
    taint_log_temps[i] = tcg_temp_new_i64();
#endif /* TCG_TARGET_REG_BITS */
#endif /* TCG_LOGGING_TAINT */

#ifdef TCG_TAINT_BRANCHING
  cc_in_use = 0;
  global_in_use = 0;
  cond_used = 0;

  /* Initialize the metadata that marks which registers
    have been initialized. */
  for (i = 0; i < tcg_ctx.nb_globals; i++)
    gen_opc_init_metadata[i] = 1;
  for (i = tcg_ctx.nb_globals; i < METADATA_SIZE; i++)
    gen_opc_init_metadata[i] = 0;
#if 0 // AWH
  /* Determine if this TB uses a brcond variant in it.  If so,
    we should implement branching on this TB. */
  for (i=0; i < nb_opc; i++)
  {
    if (cond_used) break;

    /* Check if we're using CC flags in this TB.  If so, don't 
      instrument this TB with jumps. */
    opc = gen_old_opc_buf[i];
    
    /* Determine the number and type of arguments for the opcode */
    if (opc == INDEX_op_call) {
      TCGArg arg = gen_old_opparam_buf[opparam_index];
      nb_oargs = arg >> 16;
      nb_iargs = arg & 0xffff;
      nb_cargs = tcg_op_defs[opc].nb_cargs;
      nb_args = nb_oargs + nb_iargs + nb_cargs + 1;
    } else if (opc == INDEX_op_nopn) {
      nb_args = nb_cargs = gen_old_opparam_buf[opparam_index];
      nb_oargs = nb_iargs = 0;
    } else {
      nb_args = tcg_op_defs[opc].nb_args;
      nb_oargs = tcg_op_defs[opc].nb_oargs;
      nb_iargs = tcg_op_defs[opc].nb_iargs;
      nb_cargs = tcg_op_defs[opc].nb_cargs;

      /* Are any of the input/output args CC flags? */
      for (x=0; x < nb_oargs; x++) if(check_cc_arg(gen_old_opparam_buf[opparam_index+x])) cond_used = 1;
      //for (x=0; x < nb_iargs; x++) if(check_cc_arg(??)) cond_used = 1;
    }
    opparam_index += nb_args;

    /* Check if we're using brcond* in this TB.  If so, don't
      instrument this TB with jumps. */
    switch(opc)
    {
      case INDEX_op_brcond_i32:
      //case INDEX_op_setcond_i32:
#if (TCG_TARGET_REG_BITS == 32)
      case INDEX_op_brcond2_i32:
      //case INDEX_op_setcond2_i32:
#endif /* TCG_TARGET_REG_BITS */
      case INDEX_op_brcond_i64:
        cond_used = 1;
        i = nb_opc;
        break;
    }
  }
  opparam_index = 0;
  cond_used = 0; // AWH - Testing
  //if (!cond_used) fprintf(stderr, "No cond_used in this TB\n");
  //else fprintf(stderr, "COND_USED in this TB\n");
#endif /* Remove cond check for now */
#endif /* TCG_TAINT_BRANCHING */

  /* Initialize the metadata that marks which opcodes are
    immune to being optimized out.  By default, if TCG_TAINT_BRANCHING
    is defined, none can be optimized out (all are set to 1).
    As we copy in opcodes, if the gen_old_liveness_metadata 
    for the new opcode shows that it is 0 (this opcode would 
    be optimized out anyway), then we set this to 0. If
    TCG_TAINT_BRANCHING is not defined, then all can be
    optimized out (all are set to 0). */
  for (i=0; i < METADATA_SIZE; i++)
#ifdef TCG_TAINT_BRANCHING
    gen_opc_opt_immune_metadata[i] = 1;
#else
    gen_opc_opt_immune_metadata[i] = 0;
#endif /* TCG_TAINT_BRANCHING */

  /* Copy and instrument the opcodes that need taint tracking */
  while(opc_index < nb_opc) {
    /* If needed, copy all of the appropriate metadata */
    if (search_pc && (gen_old_opc_instr_start[opc_index] == 1)) {
      return_lj = gen_opc_ptr - gen_opc_buf;
      gen_opc_pc[return_lj] = gen_old_opc_pc[opc_index];
      gen_opc_instr_start[return_lj] = 1;
      gen_opc_icount[return_lj] = gen_old_opc_icount[opc_index];
#if defined(TARGET_I386)
      gen_opc_cc_op[return_lj] = gen_old_opc_cc_op[opc_index];
#elif defined(TARGET_ARM)
      gen_opc_condexec_bits[return_lj] = gen_old_opc_condexec_bits[opc_index];
#endif /* TARGET check */ 
    }

    /* Copy the opcode to be instrumented */
    opc = *(gen_opc_ptr++) = gen_old_opc_buf[opc_index++];

    /* Determine the number and type of arguments for the opcode */
    if (opc == INDEX_op_call) {
      TCGArg arg = gen_old_opparam_buf[opparam_index];
      nb_oargs = arg >> 16;
      nb_iargs = arg & 0xffff;
      nb_cargs = tcg_op_defs[opc].nb_cargs;
      nb_args = nb_oargs + nb_iargs + nb_cargs + 1;
    } else if (opc == INDEX_op_nopn) {
      nb_args = nb_cargs = gen_old_opparam_buf[opparam_index];
      nb_oargs = nb_iargs = 0;
    } else {
      nb_args = tcg_op_defs[opc].nb_args;
      nb_oargs = tcg_op_defs[opc].nb_oargs;
      nb_iargs = tcg_op_defs[opc].nb_iargs;
      nb_cargs = tcg_op_defs[opc].nb_cargs;
    }

    /* Copy the appropriate number of arguments for the opcode */
    for(i=0; i<nb_args; i++)
      *(gen_opparam_ptr++) = gen_old_opparam_buf[opparam_index++];

    /* Copy the current gen_opc_ptr.  After we instrument this IR,
       we compare the copy of gen_opc_ptr against its current value.
       If it has increased, that means we inserted additional IR and,
       if this is a "search_pc" TB, that means we know how many extra
       entries we need to put in the metadata buffers to keep
       everything in sync. */
    gen_old_opc_ptr = gen_opc_ptr;
#ifdef USE_TCG_OPTIMIZATIONS
    /* Liveness check: If the opcode that we are going to instrument
      will be eliminated in a later liveness check (according to the
      metadata held in gen_old_liveness_metadata), then we won't
      instrument it. */
    if(!gen_old_liveness_metadata[opc_index-1]) {
      /* Tell the REAL optimizer pass in tcg/tcg.c that it is OK
        to optimize this opcode out. */
      gen_opc_opt_immune_metadata[opc_index-1] = 0;
      goto skip_instrumentation;
    }
#endif /* USE_TCG_OPTIMIZATIONS */

    switch(opc)
    {
      /* The following opcodes propagate no taint */
      case INDEX_op_end:    
      case INDEX_op_nop:
      case INDEX_op_nop1:
      case INDEX_op_nop2:
      case INDEX_op_nop3:
      case INDEX_op_nopn:
      case INDEX_op_set_label:
      case INDEX_op_debug_insn_start:
      case INDEX_op_goto_tb:
      case INDEX_op_exit_tb:
      case INDEX_op_jmp:
      case INDEX_op_br:
      case INDEX_op_brcond_i32:
#if (TCG_TARGET_REG_BITS == 32)
      case INDEX_op_brcond2_i32:
#endif /* TCG_TARGET_REG_BITS */
      case INDEX_op_brcond_i64:
        break;

      case INDEX_op_discard:   // Remove associated shadow reg
        orig0 = gen_opparam_ptr[-1];
        arg0 = find_shadow_arg(gen_opparam_ptr[-1]);
        if (arg0) {
          tcg_gen_discard_tl(arg0);
        }
#ifdef TCG_TAINT_BRANCHING
        /* LHS now uninitialized */
        if (orig0 >= tcg_ctx.nb_globals)
          gen_opc_init_metadata[orig0] = 0;
#endif /* TCG_TAINT_BRANCHING */
        break;

      case INDEX_op_call:      // Always bit taint
        // Call is a bit different, because it has a constant arg 
        // that comes before the input args (if any).  That constant 
        // says how many arguments follow, since the Call op has a 
        // variable number of arguments
        // [OP][# of args breakdown(const)][arg0(I/O][arg1(I/O)]...
        //    [argN(I)][# of args (const)]
#if defined(TARGET_I386)
        // Check if this is a call to an OUT helper function.
        // If so, we need to add in a ST IR to store the proper 
        // taint value in tempidx prior to the call.
        // These calls always have five parameters.
        if (out_helper_func) {
          // Back up call arguments
          arg5 = gen_opparam_ptr[-6]; // Const/in/out encoding
          arg4 = gen_opparam_ptr[-5]; // (I) Port
          arg3 = gen_opparam_ptr[-4]; // (I) Data
          arg2 = gen_opparam_ptr[-3]; // (I) Register with function address
          arg1 = gen_opparam_ptr[-2]; // (C) Flags
          arg0 = gen_opparam_ptr[-1]; // (C) ?

          // Find the shadow for the data input
          arg6 = find_shadow_arg(gen_opparam_ptr[-4]);
          // Back up the instruction/arg stream so that we can patch
          gen_opparam_ptr -= 6;
          gen_opc_ptr--;
          // Insert the ST opcode
          if (arg6) {
            tcg_gen_st_tl(arg6, cpu_env, offsetof(OurCPUState,tempidx));
          } else {
#if TCG_TARGET_REG_BITS == 32
            t0 = tcg_temp_new_i32();
            tcg_gen_movi_i32(t0, 0);
            tcg_gen_st_tl(t0, cpu_env, offsetof(OurCPUState,tempidx));
#else
            t0 = tcg_temp_new_i64();
            tcg_gen_movi_i64(t0, 0);
            tcg_gen_st_tl(t0, cpu_env, offsetof(OurCPUState,tempidx));
#endif /* TARGET_REG_BITS == 32 */
          }
          // Manually insert the CALL opcode
          *(gen_opc_ptr++) = INDEX_op_call;
          gen_opparam_ptr += 6;
          gen_opparam_ptr[-6] = arg5;
          gen_opparam_ptr[-5] = arg4;
          gen_opparam_ptr[-4] = arg3;
          gen_opparam_ptr[-3] = arg2;
          gen_opparam_ptr[-2] = arg1;
          gen_opparam_ptr[-1] = arg0;
          // Clear the helper flag
          out_helper_func = 0;
        }
#endif /* TARGET_I386 */
        for (i=0; i < nb_oargs; i++) {
          arg0 = find_shadow_arg(gen_opparam_ptr[
            (-1 * nb_args) /* Position of first argument in opcode stream */
            + 1	/* Skip first argument (which has # of arguments breakdown) */
            + i	/* Skip to the output parm that we are interested in */
          ]);
          if (arg0) {
            orig0 = gen_opparam_ptr[(-1 * nb_args) + 1 + i];
            // Check if this is a call to an IN helper function.
            // If so, we grab the tempidx after the function call.
#ifdef TARGET_I386
            if (in_helper_func) {
              tcg_gen_ld32u_tl(arg0, cpu_env, offsetof(OurCPUState,tempidx));
              in_helper_func = 0;
            } else
#endif /* TARGET_I386 */
              tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
          }
        }
        break;

      case INDEX_op_deposit_i32: // Always bitwise taint
        arg0 = find_shadow_arg(gen_opparam_ptr[-5]); // Output
        if (arg0) {
          int pos, len; // Constant parameters

          arg1 = find_shadow_arg(gen_opparam_ptr[-4]); // Input1
          arg2 = find_shadow_arg(gen_opparam_ptr[-3]); // Input2

          /* Store opcode parms */
          orig0 = gen_opparam_ptr[-5];
          orig1 = gen_opparam_ptr[-4];
          orig2 = gen_opparam_ptr[-3];
          pos = gen_opparam_ptr[-2]; // Position of mask
          len = gen_opparam_ptr[-1]; // Length of mask

          /* Rewind the instruction stream */
          gen_opparam_ptr -= 5;
          gen_opc_ptr--;

#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_DEPOSIT_I32
          local_no_taint_label = gen_new_label();
          local_complete_label = gen_new_label();
          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();

          /* Has LHS has been initialized? */
          if (!gen_opc_init_metadata[orig0]) {
            if (arg1 && arg2)
              tcg_gen_or_i32(t0, arg1, arg2);
            else if (arg1)
              tcg_gen_mov_i32(t0, arg1);
            else if (arg2)
              tcg_gen_mov_i32(t0, arg2);
            else
              tcg_gen_movi_i32(t0, 0);
          } else {
            if (arg1 && arg2) {
              tcg_gen_or_i32(t1, arg0, arg1);
              tcg_gen_or_i32(t0, t1, arg2);
            }
            else if (arg1)
              tcg_gen_or_i32(t0, arg0, arg1);
            else if (arg2)
              tcg_gen_or_i32(t0, arg0, arg2);
            else
              tcg_gen_mov_i32(t0, arg0);
          }
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          /* Skip logging/instrumentation if LHS and RHS don't have taint */
          tcg_gen_local_brcond_i32(TCG_COND_EQ, t0, t_zero, local_no_taint_label);
#endif /* BRANCH_DEPOSIT_I32 */
#endif /* TCG_TAINT_BRANCHING */

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_DEPOSIT_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          set_arg_i32(3, pos);
          set_arg_i32(4, len);
          //set_concrete_LHS_i32(5, arg0);
          set_concrete_i32(5, arg1);
          set_concrete_i32(6, arg2);

          tcg_gen_helperN(helper_taint_log_deposit_i32, 0, 0, TCG_CALL_DUMMY_ARG, 7, helper_arg_array);
#endif /* LOG_DEPOSIT_I32 */
#endif /* TCG_LOGGING_TAINT */

          /* Insert taint IR */
          // Handle special 32-bit transfer case (copy arg2 taint)
          if (len == 32)
            tcg_gen_mov_i32(arg0, arg2);
          // Handle special 0-bit transfer case (copy arg1 taint)
          else if (len == 0)
            tcg_gen_mov_i32(arg0, arg1);
          // Handle general case
          else
            tcg_gen_deposit_tl(arg0, arg1, arg2, pos, len);
#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_DEPOSIT_I32
          tcg_gen_local_br(local_complete_label);
          gen_local_set_label(local_no_taint_label);
          tcg_gen_movi_i32(arg0, 0);
          gen_local_set_label(local_complete_label);
#endif /* BRANCH_DEPOSIT_I32 */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */

          /* Reinsert the original IR */
          tcg_gen_deposit_i32(orig0, orig1, orig2, pos, len);
        }
        break;

#if TCG_TARGET_REG_BITS == 32
      case INDEX_op_setcond2_i32: // All-Around: UifU64() w/ mkPCastTo()
        arg0 = find_shadow_arg(gen_opparam_ptr[-6]); // Output
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-5]); // Input1 low
          arg2 = find_shadow_arg(gen_opparam_ptr[-4]); // Input1 high
          arg3 = find_shadow_arg(gen_opparam_ptr[-3]); // Input2 low
          arg4 = find_shadow_arg(gen_opparam_ptr[-2]); // Input2 high

          /* Store opcode parms */
          orig0 = gen_opparam_ptr[-6];
          orig1 = gen_opparam_ptr[-5];
          orig2 = gen_opparam_ptr[-4];
          orig3 = gen_opparam_ptr[-3];
          orig4 = gen_opparam_ptr[-2];
          orig5 = gen_opparam_ptr[-1];

          /* Rewind the instruction stream */
          gen_opparam_ptr -= 6;
          gen_opc_ptr--;

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_SETCOND2_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          set_arg_i32(3, orig3);
          set_arg_i32(4, orig4);
          set_arg_i32(5, orig5);
          //set_concrete_LHS_i32(6, arg0);
          set_concrete_i32(6, arg1);
          set_concrete_i32(7, arg2);
          set_concrete_i32(8, arg3);
          set_concrete_i32(9, arg4);

          tcg_gen_helperN(helper_taint_log_setcond2_i32, 0, 0, TCG_CALL_DUMMY_ARG, 10, helper_arg_array);
#endif /* LOG_SETCOND2_I32 */
#endif /* TCG_LOGGING_TAINT */

          /* Insert taint IR */
          // Combine high/low taint of Input 1 into t2
          t2 = tcg_temp_new_i32();
          if (arg1 && arg2)
            tcg_gen_or_i32(t2, arg1, arg2);
          else if (arg1)
            tcg_gen_mov_i32(t2, arg1);
          else if (arg2)
            tcg_gen_mov_i32(t2, arg2);
          else
            tcg_gen_movi_i32(t2, 0);

          // Combine high/low taint of Input 2 into t3
          t3 = tcg_temp_new_i32();
          if (arg3 && arg4)
            tcg_gen_or_i32(t3, arg3, arg4);
          else if (arg3)
            tcg_gen_mov_i32(t3, arg3);
          else if (arg4)
            tcg_gen_mov_i32(t3, arg4);
          else
            tcg_gen_movi_i32(t3, 0);

          // Determine if there is any taint
          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          tcg_gen_or_i32(t0, t2, t3);
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_setcond_i32(TCG_COND_NE, t2, t0, t_zero); // Reuse t2
          tcg_gen_neg_i32(arg0, t2);

#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */

          /* Reinsert original opcode */
          tcg_gen_op6i_i32(INDEX_op_setcond2_i32, orig0, orig1, orig2, orig3, orig4, orig5);
        }
        break;
#endif /* TCG_TARGET_REG_BITS */

      case INDEX_op_movi_i32: // Always bit taint
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
#ifdef TARGET_I386
          /* Check if the constant is a helper function for IN* opcodes */
          if ( (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_inb) ||
            (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_inw) ||
            (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_inl) )
          {
            in_helper_func = 1;
            //fprintf(stderr, "tcg_taint.c: movi_i32 in helper func\n");
          }
          /* Check if the constant is a helper function for OUT* opcodes */
          else if ( (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_outb) ||
            (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_outw) ||
            (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_outl) )
          {
            out_helper_func = 1;
            //fprintf(stderr, "tcg_taint.c: movi_i32 out helper func\n");
          }
#endif /* TARGET_I386 */
          /* Store opcode parms */
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];

          /* Rewind the instruction stream */
          gen_opparam_ptr -= 2;
          gen_opc_ptr--;

#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_MOVI_I32
          local_no_taint_label = gen_new_label();
          /* Skip logging/instrumentation if LHS has not yet been used */
          if (!gen_opc_init_metadata[orig0]) {
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
            tcg_gen_movi_i32(arg0, 0);
            /* Reinsert original opcode */
            tcg_gen_movi_i32(orig0, orig1);
            break;
          }
            
          /* Skip logging/instrumentation if LHS doesn't have taint */
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_local_brcond_i32(TCG_COND_EQ, arg0, t_zero, local_no_taint_label);
#endif /* BRANCH_MOVI_I32 */
#endif /* TCG_TAINT_BRANCHING */

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_MOVI_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          //set_concrete_LHS_i32(1, arg0); 
          tcg_gen_helperN(helper_taint_log_movi_i32, 0, 0, TCG_CALL_DUMMY_ARG, 1, helper_arg_array);
#endif /* LOG_MOVI_I32 */
#endif /* TCG_LOGGING_TAINT */

          /* Insert taint propagation */
          tcg_gen_movi_i32(arg0, 0);

#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_MOVI_I32
          gen_local_set_label(local_no_taint_label);
#endif /* BRANCH_MOVI_I32 */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
          /* Reinsert original opcode */
          tcg_gen_movi_i32(orig0, orig1);
        }
        break;

      case INDEX_op_mov_i32:    // Always bit taint
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
        if (arg0) {
          /* Store opcode parms */
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];

          /* Rewind the instruction stream */
          gen_opparam_ptr -= 2;
          gen_opc_ptr--;

#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_MOV_I32
#ifdef ALLOW_BSLE // AWH - DEBUG
if (block_count <= BLOCK_SKIP_LESS_EQUAL) goto skip1;
#endif // ALLOW_BSLE
#ifdef ALLOW_BSGE // AWH - DEBUG
if (block_count >= BLOCK_SKIP_GREATER_EQUAL) goto skip1;
#endif // ALLOW_BSGE
#if 1 // AWH - Custom skip
if (opc_index < 11) goto skip1;
#endif
          local_no_taint_label = gen_new_label();
          local_complete_label = gen_new_label();
          //if (!cond_used) {
            t_zero = tcg_temp_new_i32();
            tcg_gen_movi_i32(t_zero, 0);
            /* Has LHS has been initialized? */
            if (!gen_opc_init_metadata[orig0]) {
              tcg_gen_local_brcond_i32(TCG_COND_EQ, arg1, t_zero, local_no_taint_label);
            } else {
              t0 = tcg_temp_new_i32();
              tcg_gen_or_i32(t0, arg0, arg1);
              /* Skip logging/instrumentation if LHS and RHS don't have taint */
              tcg_gen_local_brcond_i32(TCG_COND_EQ, t0, t_zero, local_no_taint_label);
            }
          //}
skip1:

#endif /* BRANCH_MOV_I32 */
#endif /* TCG_TAINT_BRANCHING */

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_MOV_I32 
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          //set_concrete_LHS_i32(2, arg0);
          set_concrete_i32(2, arg1);
          tcg_gen_helperN(helper_taint_log_mov_i32, 0, 0, TCG_CALL_DUMMY_ARG, 3, helper_arg_array);
#endif /* LOG_MOV_I32 */
#endif /* TCG_LOGGING_TAINT */

          /* Insert taint propagation */
          tcg_gen_mov_i32(arg0, arg1);

#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_MOV_I32
#ifdef ALLOW_BSLE // AWH - DEBUG
if (block_count <= BLOCK_SKIP_LESS_EQUAL) goto skip2;
#endif // ALLOW_BSLE
#ifdef ALLOW_BSGE // AWH - DEBUG
if (block_count >= BLOCK_SKIP_GREATER_EQUAL) goto skip2;
#endif // ALLOW_BSGE
#if 1 // AWH - Custom skip
if (opc_index < 11) goto skip2;
#endif
          //if (cond_used) {
          //  tcg_gen_movi_i32(arg0, 0); 
          //} else 
          //{
            tcg_gen_local_br(local_complete_label);
            gen_local_set_label(local_no_taint_label);
            tcg_gen_movi_i32(arg0, 0);
            gen_local_set_label(local_complete_label);
          //}
#endif /* BRANCH_MOV_I32 */
skip2:
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */

          /* Reinsert original opcode */
          tcg_gen_mov_i32(orig0, orig1);
        }
        break;

      /* Load/store operations (32 bit). */
      /* MemCheck: mkLazyN() (Just load/store taint from/to memory) */
      case INDEX_op_qemu_ld8u: 
      case INDEX_op_qemu_ld8s: 
      case INDEX_op_qemu_ld16u:
      case INDEX_op_qemu_ld16s:
#if TCG_TARGET_REG_BITS == 64
      case INDEX_op_qemu_ld32u:
      case INDEX_op_qemu_ld32s:
#endif /* TCG_TARGET_REG_BITS == 64 */
      case INDEX_op_qemu_ld32:
        // TARGET_REG_BITS = 64 OR (TARGET_REG_BITS = 32, TARGET_LONG_BITS = 32)
        if (nb_iargs == 1) arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        // TARGET_REG_BITS = 32, TARGET_LONG_BITS = 64
        else tcg_abort(); // Not supported
        if (arg0) {
          /* Patch qemu_ld* opcode into taint_qemu_ld* */
          gen_opc_ptr[-1] += (INDEX_op_taint_qemu_ld8u - INDEX_op_qemu_ld8u);
          orig0 = gen_opparam_ptr[-3];
          /* Are we doing pointer tainting? */
          if (taint_load_pointers_enabled) {
            arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
            int addr = gen_opparam_ptr[-2];
            if (arg1) {
#ifdef LOG_POINTER
                set_con_i32(0, addr);
                set_con_i32(1, arg1);
                tcg_gen_helperN(helper_taint_log_pointer, 0, 0, TCG_CALL_DUMMY_ARG, 2, helper_arg_array);

#endif

#if (TCG_TARGET_REG_BITS == 64)
                t0 = tcg_temp_new_i64();
                t1 = tcg_temp_new_i64();
                t2 = tcg_temp_new_i64();
                t3 = tcg_temp_new_i64();

                /* Load taint from tempidx */
                tcg_gen_ld32u_tl(t3, cpu_env, offsetof(OurCPUState,tempidx));

#ifndef TAINT_NEW_POINTER //more selective pointer tainting
                /* Check for pointer taint */
                t_zero = tcg_temp_new_i64();
                tcg_gen_movi_i64(t_zero, 0);
                tcg_gen_setcond_i64(TCG_COND_NE, t2, arg1, t_zero);
#else
                t4 = tcg_temp_new_i64();
                tcg_gen_movi_i64(t2, 0xffff0000);
                tcg_gen_and_i64(t0, arg1, t2);//t0 = H_taint
                tcg_gen_movi_i64(t2, 0);
                tcg_gen_setcond_i64(TCG_COND_EQ, t1, t0, t2);  //t1=(H_taint==0) cond1
                tcg_gen_setcond_i64(TCG_COND_NE, t4, arg1, t2);  //t4=(P_taint!=0) cond2
                tcg_gen_and_i64(t2, t1, t4); //t2 = cond1 & cond2
#endif
                tcg_gen_neg_i64(t0, t2);

                /* Combine pointer and tempidx taint */
                tcg_gen_or_i64(arg0, t0, t3);

#else
              t0 = tcg_temp_new_i32();
              t1 = tcg_temp_new_i32();
              t2 = tcg_temp_new_i32();
              t3 = tcg_temp_new_i32();
              /* Load taint from tempidx */
              tcg_gen_ld_i32(t3, cpu_env, offsetof(OurCPUState,tempidx));
              /* Check for pointer taint */
#ifndef TAINT_NEW_POINTER
              t_zero = tcg_temp_new_i32();
              tcg_gen_movi_i32(t_zero, 0);
              tcg_gen_setcond_i32(TCG_COND_NE, t2, arg1, t_zero);
#else
              t4 = tcg_temp_new_i32();
              tcg_gen_movi_i32(t2, 0xffff0000); //??
              tcg_gen_and_i32(t0, arg1, t2);//t0 = H_taint
              tcg_gen_movi_i32(t2, 0);
              tcg_gen_setcond_i32(TCG_COND_EQ, t1, t0, t2);  //t1=(H_taint==0) cond1
              tcg_gen_setcond_i32(TCG_COND_NE, t4, arg1, t2);  //t4=(P_taint!=0) cond2
              tcg_gen_and_i32(t2, t1, t4); //t2 = cond1 & cond2
#endif
              tcg_gen_neg_i32(t0, t2);
              /* Combine pointer and tempidx taint */
              tcg_gen_or_i32(arg0, t0, t3);
#endif /* TARGET_REG_BITS */
            } else
              /* Patch in opcode to load taint from tempidx */
              tcg_gen_ld_i32(arg0, cpu_env, offsetof(OurCPUState,tempidx));
          } else
            /* Patch in opcode to load taint from tempidx */
            tcg_gen_ld_i32(arg0, cpu_env, offsetof(OurCPUState,tempidx));
#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;

      case INDEX_op_qemu_ld64:
#if 1 // AWH - FIXME: 64-bit memory ops may cause corruption
        DUMMY_TAINT(nb_oargs, nb_args);
#else
        // TARGET_REG_BITS = 32, TARGET_LONG_BITS = 32
        if ((nb_oargs == 2) && (nb_iargs == 1)) {
          arg0 = find_shadow_arg(gen_opparam_ptr[-4]); // Taint of low DWORD
          arg1 = find_shadow_arg(gen_opparam_ptr[-3]); // Taint of hi DWORD
          if (arg0 || arg1) {
            gen_opc_ptr[-1] += (INDEX_op_taint_qemu_ld8u - INDEX_op_qemu_ld8u);
            if (taint_load_pointers_enabled) {
              arg2 = find_shadow_arg(gen_opparam_ptr[-2]);
              if (arg2) {
                t0 = tcg_temp_new_i32();
                t1 = tcg_temp_new_i32();
                t2 = tcg_temp_new_i32();
                t3 = tcg_temp_new_i32();
 
                /* Load taint from tempidx */
                tcg_gen_ld_i32(t2, cpu_env, offsetof(OurCPUState,tempidx));
                tcg_gen_ld_i32(t3, cpu_env, offsetof(OurCPUState, tempidx2));

                /* Check for pointer taint */
                t_zero = tcg_temp_new_i32();
                tcg_gen_movi_i32(t_zero, 0);
                tcg_gen_setcond_i32(TCG_COND_NE, t1, arg2, t_zero);
                tcg_gen_neg_i32(t0, t1);

                /* Combine pointer and tempidx taint */
                if (arg0)
                  tcg_gen_or_i32(arg0, t0, t2);
                if (arg1)
                  tcg_gen_or_i32(arg1, t0, t3);

              } else {
                /* Patch in opcode to load taint from tempidx */
                if (arg0)
                  tcg_gen_ld_i32(arg0, cpu_env, offsetof(OurCPUState,tempidx));
                if (arg1)
                  tcg_gen_ld_i32(arg1, cpu_env, offsetof(OurCPUState,tempidx2));
              }
            }  /* taint_pointers_enabled */ 
            else {
              /* Patch in opcode to load taint from tempidx */
              if (arg0)
                tcg_gen_ld32u_tl(arg0, cpu_env, offsetof(OurCPUState,tempidx));
              if (arg1)
                tcg_gen_ld32u_tl(arg1, cpu_env, offsetof(OurCPUState,tempidx2));
            } /* taint_pointers_enabled */
          }
        // TARGET_REG_BITS = 64, TARGET_LONG_BITS = 64
        } else if ((nb_oargs ==1) && (nb_iargs == 1)) {
          arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
          if (arg0) {
            gen_opc_ptr[-1] += (INDEX_op_taint_qemu_ld8u - INDEX_op_qemu_ld8u);
            if (taint_load_pointers_enabled) {
              arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
              if (arg1) {
                t0 = tcg_temp_new_i64();
                t1 = tcg_temp_new_i64();
                t2 = tcg_temp_new_i64();
                t3 = tcg_temp_new_i64();

                /* Load taint from tempidx */
                tcg_gen_ld_i64(t3, cpu_env, offsetof(OurCPUState,tempidx));

                /* Check for pointer taint */
                t_zero = tcg_temp_new_i64();
                tcg_gen_movi_i64(t_zero, 0);
                tcg_gen_setcond_i64(TCG_COND_NE, t2, arg1, t_zero);
                tcg_gen_neg_i64(t0, t2);

                /* Combine pointer and tempidx taint */
                tcg_gen_or_i64(arg0, t0, t3);
              } else
                /* Patch in opcode to load taint from tempidx */
                tcg_gen_ld_i64(arg0, cpu_env, offsetof(OurCPUState,tempidx));
            } else
              /* Patch in opcode to load taint from tempidx */
              tcg_gen_ld_i64(arg0, cpu_env, offsetof(OurCPUState,tempidx));
          }
        // TARGET_REG_BITS = 64, TARGET_LONG_BITS = 32
        } else
          tcg_abort();
#endif // FIXME
        break;

#if 1 // AWH - DEBUG
      case INDEX_op_qemu_st32:
        //DUMMY_TAINT(nb_oargs, nb_args);
        //break;
 
      case INDEX_op_qemu_st8:
      case INDEX_op_qemu_st16:
#else
      case INDEX_op_qemu_st8: 
      case INDEX_op_qemu_st16:
      case INDEX_op_qemu_st32:
#endif // AWH
        // TARGET_REG_BITS = 64 OR (TARGET_REG_BITS = 32, TARGET_LONG_BITS = 32)
        if (nb_iargs == 2) {
          arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          if (arg0) {
            /* Save the qemu_st* parameters */
            int mem_index = gen_opparam_ptr[-1];
            int addr = gen_opparam_ptr[-2];
            int ret = gen_opparam_ptr[-3];
            int ir = gen_opc_ptr[-1];
            /* Back up to insert a new IR on top of the qemu_st*  */
            gen_opc_ptr--;
            gen_opparam_ptr -= 3;

            if (taint_store_pointers_enabled) {
              if (arg1) {

#if (TCG_TARGET_REG_BITS == 64)
                t0 = tcg_temp_new_i64();
                t1 = tcg_temp_new_i64();
                t2 = tcg_temp_new_i64();
                
                /* Check for pointer taint */
                t_zero = tcg_temp_new_i64();
                tcg_gen_movi_i64(t_zero, 0);
                tcg_gen_setcond_i64(TCG_COND_NE, t2, arg1, t_zero);
                tcg_gen_neg_i64(t0, t2);
                /* Combine pointer and data taint */
                tcg_gen_or_i64(t1, t0, arg0);
                /* Store combined taint to tempidx */
                tcg_gen_st32_tl(t1, cpu_env, offsetof(OurCPUState,tempidx));
#else
                t0 = tcg_temp_new_i32();
                t1 = tcg_temp_new_i32();
                t2 = tcg_temp_new_i32();

                /* Check for pointer taint */
                t_zero = tcg_temp_new_i32();
                tcg_gen_movi_i32(t_zero, 0);
                tcg_gen_setcond_i32(TCG_COND_NE, t2, arg1, t_zero);
                tcg_gen_neg_i32(t0, t2);
                /* Combine pointer and data taint */
                tcg_gen_or_i32(t1, t0, arg0);
                /* Store combined taint to tempidx */
                tcg_gen_st32_tl(t1, cpu_env, offsetof(OurCPUState,tempidx));
#endif /* TARGET_REG_BITS */

              } else
                tcg_gen_st32_tl(arg0, cpu_env, offsetof(OurCPUState,tempidx));
            } else
              tcg_gen_st32_tl(arg0, cpu_env, offsetof(OurCPUState,tempidx));

            /* Insert the taint_qemu_st* IR */
            gen_opc_ptr++;
            gen_opparam_ptr += 3;
            gen_opc_ptr[-1] = ir + (INDEX_op_taint_qemu_ld8u - INDEX_op_qemu_ld8u);
            gen_opparam_ptr[-1] = mem_index; 
            gen_opparam_ptr[-2] = addr;
            gen_opparam_ptr[-3] = ret;
          }
        } else
          tcg_abort();
        break;

      case INDEX_op_qemu_st64:
#if 0 // AWH - FIXME: 64-bit memory ops may cause corruption
        /* TARGET_REG_BITS == 64 */
        if (nb_iargs == 2) {
          arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
          if (arg0) {
            /* Save the qemu_st64 parameters */
            int mem_index = gen_opparam_ptr[-1];
            int addr = gen_opparam_ptr[-2];
            int ret = gen_opparam_ptr[-3];

            /* Back up to insert a new IR on top of the qemu_st64 */
            gen_opc_ptr--;
            gen_opparam_ptr -= 3;

            if (taint_store_pointers_enabled) {
              arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
              if (arg1) {
                t0 = tcg_temp_new_i64();
                t1 = tcg_temp_new_i64();
                t2 = tcg_temp_new_i64();

                /* Check for pointer taint */
                tcg_gen_movi_i64(t1, 0);
                tcg_gen_setcond_i64(TCG_COND_NE, t2, arg1, t1);
                tcg_gen_neg_i64(t0, t2);
                /* Combine pointer and data taint */
                tcg_gen_or_i64(t1, t0, arg0);
                /* Store combined taint to tempidx */
                tcg_gen_st_tl(t1, cpu_env, offsetof(OurCPUState,tempidx));

                //tcg_temp_free_i64(t0);
                //tcg_temp_free_i64(t1);
                //tcg_temp_free_i64(t2);
              } else
                tcg_gen_st_tl(arg0, cpu_env, offsetof(OurCPUState,tempidx));
            } else
              tcg_gen_st_tl(arg0, cpu_env, offsetof(OurCPUState,tempidx));

            /* Insert the taint_qemu_st* IR */
            gen_opc_ptr++;
            gen_opparam_ptr += 3;

            gen_opc_ptr[-1] = INDEX_op_taint_qemu_st64;
            gen_opparam_ptr[-1] = mem_index;
            gen_opparam_ptr[-2] = addr;
            gen_opparam_ptr[-3] = ret;
          }
        // TARGET_REG_BITS = 32, TARGET_LONG_BITS = 32
        } else if (nb_iargs == 3) {
          arg0 = find_shadow_arg(gen_opparam_ptr[-4]); // Taint of low DWORD
          arg1 = find_shadow_arg(gen_opparam_ptr[-3]); // Taint of high DWORD
          if (arg0 || arg1) {
            int ret_lo = gen_opparam_ptr[-4]; // Low DWORD of data
            int ret_hi = gen_opparam_ptr[-3]; // High DWORD of Data
            int addr = gen_opparam_ptr[-2]; // Addr
            int mem_index = gen_opparam_ptr[-1]; // MMU index

            /* Back up to insert two new store IRs on top of the qemu_st64 */
            gen_opc_ptr--;
            gen_opparam_ptr -= 4;

            t0 = tcg_temp_new_i32();
            t1 = tcg_temp_new_i32();
            t2 = tcg_temp_new_i32();

            if (taint_store_pointers_enabled) {
              arg2 = find_shadow_arg(gen_opparam_ptr[-2]);
              if (arg2) {
                /* Check for pointer taint */
                tcg_gen_movi_i32(t1, 0);
                tcg_gen_setcond_i32(TCG_COND_NE, t2, arg1, t1);
                tcg_gen_neg_i32(t0, t2);
                /* Combine pointer and data taint */
                if (!arg0)
                  tcg_gen_st_tl(t0, cpu_env, offsetof(OurCPUState,tempidx));
                else {
                  tcg_gen_or_i32(t1, t0, arg0);
                  tcg_gen_st_tl(t1, cpu_env, offsetof(OurCPUState,tempidx));
                }

                if (!arg1)
                  tcg_gen_st_tl(t0, cpu_env, offsetof(OurCPUState,tempidx2));
                else {
                  tcg_gen_or_i32(t1, t0, arg1);
                  tcg_gen_st_tl(t1, cpu_env, offsetof(OurCPUState,tempidx2));
                }

              } else {
                /* If there is no shadow data for either one of the 32-bit chunks
                that make up this 64-bit store, then use a zeroed-out temp reg
                to indicate there is no taint for that 32-bit chunk. */
                if (!arg0) {
                  tcg_gen_movi_i32(t0, 0);
                  tcg_gen_st_tl(t0, cpu_env, offsetof(OurCPUState,tempidx));
                } else
                  tcg_gen_st_tl(arg0, cpu_env, offsetof(OurCPUState,tempidx));

                if (!arg1) {
                  tcg_gen_movi_i32(t1, 0);
                  tcg_gen_st_tl(t1, cpu_env, offsetof(OurCPUState,tempidx2));
                } else
                  tcg_gen_st_tl(arg1, cpu_env, offsetof(OurCPUState,tempidx2));
              }

              //tcg_temp_free_i32(t0);
              //tcg_temp_free_i32(t1);
              //tcg_temp_free_i32(t2);
            } else {
              /* If there is no shadow data for either one of the 32-bit chunks
              that make up this 64-bit store, then use a zeroed-out temp reg
              to indicate there is no taint for that 32-bit chunk. */
              if (!arg0) {
                t0 = tcg_temp_new_i32();
                tcg_gen_movi_i32(t0, 0);
                tcg_gen_st_tl(t0, cpu_env, offsetof(OurCPUState,tempidx));
                //tcg_temp_free_i32(t0);
              } else
                tcg_gen_st_tl(arg0, cpu_env, offsetof(OurCPUState,tempidx));
             
              if (!arg1) {
                t0 = tcg_temp_new_i32();
                tcg_gen_movi_i32(t0, 0);
                tcg_gen_st_tl(t0, cpu_env, offsetof(OurCPUState,tempidx2));
                //tcg_temp_free_i32(t0);
              } else
                tcg_gen_st_tl(arg1, cpu_env, offsetof(OurCPUState,tempidx2));
            }

            /* Insert the taint_qemu_st* IR */
            gen_opc_ptr++;
            gen_opparam_ptr += 4;

            gen_opc_ptr[-1] = INDEX_op_taint_qemu_st64;
            gen_opparam_ptr[-1] = mem_index;
            gen_opparam_ptr[-2] = addr;
            gen_opparam_ptr[-3] = ret_hi;
            gen_opparam_ptr[-4] = ret_lo;
          }
        // TARGET_REG_BITS = 32, TARGET_LONG_BITS = 64
        } else /*if (nb_iargs == 4)*/ {
          tcg_abort();
        }
#endif // FIXME
        break;

      /* Arithmethic/shift/rotate operations (32 bit). */
      case INDEX_op_setcond_i32: // All-Around: UifU32() (mkLazy())
        arg0 = find_shadow_arg(gen_opparam_ptr[-4]); // Output
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-3]); // Input1
          arg2 = find_shadow_arg(gen_opparam_ptr[-2]); // Input2

          /* Store opcode and parms and back up */
          orig0 = gen_opparam_ptr[-4];
          orig1 = gen_opparam_ptr[-3];
          orig2 = gen_opparam_ptr[-2];
          orig3 = gen_opparam_ptr[-1];

          /* Rewind instruction stream */
          gen_opparam_ptr -= 4;
          gen_opc_ptr--;

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_DEPOSIT_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          set_arg_i32(3, orig3);
          //set_concrete_LHS_i32(4, arg0);
          set_concrete_i32(4, arg1);
          set_concrete_i32(5, arg2);

          tcg_gen_helperN(helper_taint_log_setcond_i32, 0, 0, TCG_CALL_DUMMY_ARG, 6, helper_arg_array);
#endif /* LOG_DEPOSIT_I32 */
#endif /* TCG_LOGGING_TAINT */

          if (arg1 && arg2) {
            t0 = tcg_temp_new_i32();
            tcg_gen_or_i32(t0, arg1, arg2);
          } else if (arg1) {
            t0 = tcg_temp_new_i32();
            tcg_gen_mov_i32(t0, arg1);
          } else if (arg2) {
            t0 = tcg_temp_new_i32();
            tcg_gen_mov_i32(t0, arg2);
          } else {
            tcg_gen_mov_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
            /* Reinsert original opcode */
            tcg_gen_setcond_i32(orig0, orig1, orig2, orig3);
            break;
          }

          // Determine if there is any taint
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_setcond_i32(TCG_COND_NE, t2, t0, t_zero);
          tcg_gen_neg_i32(arg0, t2);

#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */

          /* Reinsert original opcode */
          tcg_gen_setcond_i32(orig0, orig1, orig2, orig3);
        }
        break;

      /* IN MEMCHECK (VALGRIND), LOOK AT: memcheck/mc_translate.c
         expr2vbits_Binop(), expr2vbits_Unop() */ 
      case INDEX_op_shl_i32: // Special - scalarShift()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          /* Store opcode parms */
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          /* Rewind instruction stream */
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_SHL_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          //set_concrete_LHS_i32(3, arg0);
          set_concrete_i32(3, arg1);
          set_concrete_i32(4, arg2);

          tcg_gen_helperN(helper_taint_log_shl_i32, 0, 0, TCG_CALL_DUMMY_ARG, 5, helper_arg_array);
#endif /* LOG_SHL_I32 */
#endif /* TCG_LOGGING_TAINT */

          /* Insert taint IR */
          if (!arg1 && !arg2) {
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
            /* Reinsert original opcode */
            tcg_gen_shl_i32(orig0, orig1, orig2);
            break;
          }

          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();

          if (arg2) { 
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            t_zero = tcg_temp_new_i32();
            tcg_gen_movi_i32(t_zero, 0);
            tcg_gen_setcond_i32(TCG_COND_NE, t1, t_zero, arg2);
            tcg_gen_neg_i32(t2, t1);
          } else
            tcg_gen_movi_i32(t2, 0);

          if (arg1) {
            // Perform the SHL on arg1
            tcg_gen_shl_i32(t0, arg1, orig2);//tcg_gen_shl_i32(t0, arg1, gen_opparam_ptr[-1]);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i32(arg0, t0, t2);
          } else
            tcg_gen_mov_i32(arg0, t2);
#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
          /* Reinsert original opcode */
          tcg_gen_shl_i32(orig0, orig1, orig2);
        }
        break;

      case INDEX_op_shr_i32: // Special - scalarShift()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          /* Store opcode parms */
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          /* Rewind instruction stream */
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_SHR_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          //set_concrete_LHS_i32(3, arg0);
          set_concrete_i32(3, arg1);
          set_concrete_i32(4, arg2);

          tcg_gen_helperN(helper_taint_log_shr_i32, 0, 0, TCG_CALL_DUMMY_ARG, 5, helper_arg_array);
#endif /* LOG_SHR_I32 */
#endif /* TCG_LOGGING_TAINT */

          /* Insert taint IR */
          if (!arg1 && !arg2) {
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
            /* Reinsert original opcode */
            tcg_gen_shr_i32(orig0, orig1, orig2);
            break;
          }

          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();

          if (arg2) { 
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            t_zero = tcg_temp_new_i32();
            tcg_gen_movi_i32(t_zero, 0);
            tcg_gen_setcond_i32(TCG_COND_NE, t1, t_zero, arg2);
            tcg_gen_neg_i32(t2, t1);
          } else
            tcg_gen_movi_i32(t2, 0);

          if (arg1) {
            // Perform the SHR on arg1
            tcg_gen_shr_i32(t0, arg1, orig2);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i32(arg0, t0, t2);
          } else
            tcg_gen_mov_i32(arg0, t2);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
          /* Reinsert original opcode */
          tcg_gen_shr_i32(orig0, orig1, orig2);
        }
        break;

      case INDEX_op_sar_i32: // Special - scalarShift()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          /* Store opcode parms */
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          /* Rewind the instruction stream */
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_SAR_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          //set_concrete_LHS_i32(3, arg0);
          set_concrete_i32(3, arg1);
          set_concrete_i32(4, arg2);

          tcg_gen_helperN(helper_taint_log_sar_i32, 0, 0, TCG_CALL_DUMMY_ARG, 5, helper_arg_array);
#endif /* LOG_SAR_I32 */
#endif /* TCG_LOGGING_TAINT */

          /* Insert taint IR */
          if (!arg1 && !arg2) {
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
            /* Reinsert original opcode */
            tcg_gen_sar_i32(orig0, orig1, orig2);
            break;
          }

          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();

          if (arg2) { 
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            t_zero = tcg_temp_new_i32();
            tcg_gen_movi_i32(t_zero, 0);
            tcg_gen_setcond_i32(TCG_COND_NE, t1, t_zero, arg2);
            tcg_gen_neg_i32(t2, t1);
          } else
            tcg_gen_movi_i32(t2, 0);

          if (arg1) {
            // Perform the SAR on arg1
            tcg_gen_sar_i32(t0, arg1, orig2);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i32(arg0, t0, t2);
          } else
            tcg_gen_mov_i32(arg0, t2);
#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
          /* Reinsert original opcode */
          tcg_gen_sar_i32(orig0, orig1, orig2);
        }
        break;

#if TCG_TARGET_HAS_rot_i32
      case INDEX_op_rotl_i32: // Special - MemCheck does lazy, but we shift
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          /* Store opcode and parms and back up */
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          /* Rewind the instruction stream */
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_ROTL_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          //set_concrete_LHS_i32(3, arg0);
          set_concrete_i32(3, arg1);
          set_concrete_i32(4, arg2);

          tcg_gen_helperN(helper_taint_log_rotl_i32, 0, 0, TCG_CALL_DUMMY_ARG, 5, helper_arg_array);
#endif /* LOG_ROTL_I32 */
#endif /* TCG_LOGGING_TAINT */
 
          /* Insert tainting IR */
          if (!arg1 && !arg2) {
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
            /* Reinsert original opcode */
            tcg_gen_rotl_i32(orig0, orig1, orig2);
            break;
          }

          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();

          if (arg2) { 
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            t_zero = tcg_temp_new_i32();
            tcg_gen_movi_i32(t_zero, 0);
            tcg_gen_setcond_i32(TCG_COND_NE, t1, t_zero, arg2);
            tcg_gen_neg_i32(t2, t1);
          } else
            tcg_gen_movi_i32(t2, 0);

          if (arg1) {
            // Perform the ROTL on arg1
            tcg_gen_rotl_i32(t0, arg1, orig2);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i32(arg0, t0, t2);
          } else
            tcg_gen_mov_i32(arg0, t2);
#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
          /* Reinsert original opcode */
          tcg_gen_rotl_i32(orig0, orig1, orig2);
        }
        break;

      case INDEX_op_rotr_i32: // Special - MemCheck does lazy, but we shift
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          /* Store opcode and parms and back up */
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          /* Rewind instruction stream */
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_ROTR_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          //set_concrete_LHS_i32(3, arg0);
          set_concrete_i32(3, arg1);
          set_concrete_i32(4, arg2);

          tcg_gen_helperN(helper_taint_log_rotr_i32, 0, 0, TCG_CALL_DUMMY_ARG, 5, helper_arg_array);
#endif /* LOG_ROTR_I32 */
#endif /* TCG_LOGGING_TAINT */

          /* Insert tainting IR */
          if (!arg1 && !arg2) {
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
            /* Reinsert original opcode */
            tcg_gen_rotr_i32(orig0, orig1, orig2);
            break;
          }
          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();

          if (arg2) { 
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            t_zero = tcg_temp_new_i32();
            tcg_gen_movi_i32(t_zero, 0);
            tcg_gen_setcond_i32(TCG_COND_NE, t1, t_zero, arg2);
            tcg_gen_neg_i32(t2, t1);
          } else
            tcg_gen_movi_i32(t2, 0);

          if (arg1) {
            // Perform the ROTR on arg1
            tcg_gen_rotr_i32(t0, arg1, orig2);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i32(arg0, t0, t2);
          } else
            tcg_gen_mov_i32(arg0, t2);
#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
          /* Reinsert original opcode */
          tcg_gen_rotr_i32(orig0, orig1, orig2);
        }
        break;

#endif /* TCG_TARGET_HAS_rot_i32 */
#ifdef BITWISE_TAINT
#ifdef TAINT_EXPENSIVE_ADDSUB
 // AWH - expensiveAddSub() for add_i32/or_i32 are buggy, use cheap one
      /* T0 = (T1 | T2) | ((V1_min + V2_min) ^ (V1_max + V2_max)) */
      case INDEX_op_add_i32: // Special - expensiveAddSub()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          //LOK: Changed the names of orig0 and orig 1 to orig1 and 2
          // so I don't get confused
          // Basically arg is vxx and orig is x
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          //make sure we have a copy of the values first
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          //delete the original operation
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

          
          //LOK: Declared the new temporary variables that we need
          t0 = tcg_temp_new_i32(); //scratch
          t1 = tcg_temp_new_i32(); //a_min
          t2 = tcg_temp_new_i32(); //b_min
          t3 = tcg_temp_new_i32(); //a_max
          t4 = tcg_temp_new_i32(); //b_max

          /* Per the expensiveAddSub() logic:
             qaa = T1 = arg1
             qbb = T2 = arg2
             aa  = V1 = orig1
             bb  = V2 = orig2 */

          //LOK: First lets calculate a_min = aa & ~qaa
          tcg_gen_not_i32(t0, arg1); // ~qaa
          tcg_gen_and_i32(t1, orig1, t0);//t1 = aa & ~qaa
          
          //LOK: Then calculate b_min
          tcg_gen_not_i32(t0, arg2); // ~qbb
          tcg_gen_and_i32(t2, orig2, t0);//t2 = bb & ~qbb

          //LOK: Then calculate a_max = aa | qaa
          tcg_gen_or_i32(t3, orig1, arg1);//t3 = aa | qaa
          tcg_gen_or_i32(t4, orig2, arg2);//t4 = bb | qbb

          //LOK: Now that we have the mins and maxes, we need to sum them
          tcg_gen_add_i32(t0, t3, t4); // t0 = a_max + b_max
          //LOK: Note that t3 is being reused in this case
          tcg_gen_add_i32(t3, t1, t2); // t3 = a_min + b_min
          tcg_gen_xor_i32(t1, t0, t3); // t1 = ((a_min + b_min)^(a_max + b_max))
          tcg_gen_or_i32(t0, arg1, arg2); // t0 = qa | qb
          tcg_gen_or_i32(arg0, t0, t1); // arg0 = (qa | qb) | ( (a_min + b_min) ^ (a_max + b_max)
          //put the original operation back
          tcg_gen_add_i32(orig0, orig1, orig2);
        }
        break;
      /* T0 = (T1 | T2) | ((V1_min - V2_max) ^ (V1_max - V2_min)) */
      case INDEX_op_sub_i32: // Special - expensiveAddSub()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          //NOTE: It is important that we get the order of the operands correct
          // Right now, the assumption is
          // arg0 = arg1 - arg2
          // If there are errors - this could be the culprit

          //LOK: Changed the names of orig0 and orig 1 to orig1 and 2
          // so I don't get confused
          // Basically arg is vxx and orig is x
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          //make sure we have a copy of the values first
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          //delete the original operation
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

          //LOK: Declared the new temporary variables that we need
          t0 = tcg_temp_new_i32(); //scratch
          t1 = tcg_temp_new_i32(); //a_min
          t2 = tcg_temp_new_i32(); //b_min
          t3 = tcg_temp_new_i32(); //a_max
          t4 = tcg_temp_new_i32(); //b_max

          /* Per the expensiveAddSub() logic:
             qaa = T1 = arg1
             qbb = T2 = arg2
             aa  = V1 = orig1
             bb  = V2 = orig2 */

          //LOK: First lets calculate a_min = aa & ~qaa
          tcg_gen_not_i32(t0, arg1); // ~qaa
          tcg_gen_and_i32(t1, orig1, t0);//t1 = aa & ~qaa
          
          //LOK: Then calculate b_min
          tcg_gen_not_i32(t0, arg2); // ~qbb
          tcg_gen_and_i32(t2, orig2, t0);//t2 = bb & ~qbb

          //LOK: Then calculate a_max = aa | qaa
          tcg_gen_or_i32(t3, orig1, arg1);//t3 = aa | qaa
          tcg_gen_or_i32(t4, orig2, arg2);//t4 = bb | qbb

          //LOK: Now that we have the mins and maxes, we need to find the differences
          //NOTE: This is why the order of the operands is important
          tcg_gen_sub_i32(t0, t1, t4); // t0 = a_min - b_max
          //LOK: Note that t3 is being reused in this case
          tcg_gen_sub_i32(t4, t3, t2); // t4 = a_max - b_min
          tcg_gen_xor_i32(t1, t0, t4); // t1 = ((a_min - b_max)^(a_max - b_min))
          tcg_gen_or_i32(t0, arg1, arg2); // t0 = qa | qb
          tcg_gen_or_i32(arg0, t0, t1); // arg0 = (qa | qb) | ( (a_min - b_max) ^ (a_max - b_min)

          //put the original operation back
          tcg_gen_sub_i32(orig0, orig1, orig2);
        }
        break;
 // AWH
#else
      case INDEX_op_add_i32: // Up - cheap_AddSub32
      case INDEX_op_sub_i32: // Up - cheap_AddSub32
#endif
      case INDEX_op_mul_i32: // Up - mkUifU32(), mkLeft32(), mkPCastTo()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          /* Store opcode and parms and back up */
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          /* Rewind instruction stream */
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

#ifdef TCG_LOGGING_TAINT
#ifdef LOG_MUL_I32
          /* Insert logging */
          set_arg_i32(0, orig0);
          set_arg_i32(1, orig1);
          set_arg_i32(2, orig2);
          //set_concrete_LHS_i32(3, arg0);
          set_concrete_i32(3, arg1);
          set_concrete_i32(4, arg2);

          tcg_gen_helperN(helper_taint_log_mul_i32, 0, 0, TCG_CALL_DUMMY_ARG, 5, helper_arg_array);
#endif /* LOG_MUL_I32 */
#endif /* TCG_LOGGING_TAINT */
          if (!arg1 && !arg2) {
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
            /* Reinsert original opcode */
            tcg_gen_mul_i32(orig0, orig1, orig2);
            break;
          }

          t0 = tcg_temp_new_i32();
          if (arg1 && arg2)
            // mkUifU32(arg1, arg2)
            tcg_gen_or_i32(t0, arg1, arg2);
          else if (arg1)
            tcg_gen_movi_i32(t0, arg1);
          else if (arg2)
            tcg_gen_movi_i32(t0, arg2);
   
          // mkLeft32(t0)
          t1 = tcg_temp_new_i32(); 
          tcg_gen_neg_i32(t1, t0); // (-s32)
          tcg_gen_or_i32(arg0, t0, t1); // (s32 | (-s32)) -> vLo32
#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
          /* Reinsert original opcode */
          tcg_gen_mul_i32(orig0, orig1, orig2);
        }
        break;

      /* Bitwise AND rules:
        Taint1 Value1 Op  Taint2 Value2  ResultingTaint
        0      1      AND 1      X       1
        1      X      AND 0      1       1
        1      X      AND 1      X       1
        ... otherwise, ResultingTaint = 0
        AND: ((NOT T1) * V1 * T2) + (T1 * (NOT T2) * V2) + (T1 * T2)
      */  
      case INDEX_op_and_i32: // Special - and_or_ty()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          /* Store opcode parms */
          orig0 = gen_opparam_ptr[-1];//V1
          orig1 = gen_opparam_ptr[-2];//V2
          orig2 = gen_opparam_ptr[-3];

          /* Rewind instruction stream */
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();
          t3 = tcg_temp_new_i32();

#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_AND_I32
          local_no_taint_label = gen_new_label();
          local_complete_label = gen_new_label();

          /* Has LHS has been initialized? */
          if (!gen_opc_init_metadata[orig2]) {
            if (arg1 && arg2)
              tcg_gen_or_i32(t0, arg1, arg2);
            else if (arg1)
              tcg_gen_mov_i32(t0, arg1);
            else if (arg2)
              tcg_gen_mov_i32(t0, arg2);
            else
              tcg_gen_movi_i32(t0, 0);
          } else {
            if (arg1 && arg2) {
              tcg_gen_or_i32(t1, arg0, arg1);
              tcg_gen_or_i32(t0, t1, arg2);
            }
            else if (arg1)
              tcg_gen_or_i32(t0, arg0, arg1);
            else if (arg2)
              tcg_gen_or_i32(t0, arg0, arg2);
            else
              tcg_gen_mov_i32(t0, arg0);
          }
          /* Skip logging/instrumentation if LHS and RHS don't have taint */
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_local_brcond_i32(TCG_COND_EQ, t0, t_zero, local_no_taint_label);
#endif /* BRANCH_AND_I32 */
#endif /* TCG_TAINT_BRANCHING */
          /* T1 -> arg1
             V1 -> gen_opparam_ptr[-2]
             T2 -> arg2
             V2 -> gen_opparam_ptr[-1] */
          if (arg1)
            tcg_gen_not_i32(t0, arg1); // NOT T1
          else
            tcg_gen_movi_i32(t0, -1);
          if (arg2)
            tcg_gen_and_i32(t1,orig1,arg2);//tcg_gen_and_i32(t1, gen_opparam_ptr[-2], arg2); // V1 * T2
          else
            tcg_gen_movi_i32(t1, 0);
          tcg_gen_and_i32(t2, t0, t1); // (NOT T1) * V1 * T2

          if (arg2)
            tcg_gen_not_i32(t0, arg2); // NOT T2
          else
            tcg_gen_movi_i32(t0, -1);
          if (arg1)
            tcg_gen_and_i32(t1,arg1,orig0);//tcg_gen_and_i32(t1, arg1, gen_opparam_ptr[-1]); // T1 * V2
          else
            tcg_gen_movi_i32(t1, 0);
          tcg_gen_and_i32(t3, t0, t1); // (T1 * (NOT T2) * V2)

          if (arg1 && arg2)
            tcg_gen_and_i32(t0, arg1, arg2); // T1 * T2
          else
            tcg_gen_movi_i32(t0, 0);

          // OR it all together
          tcg_gen_or_i32(t1, t2, t3);
          tcg_gen_or_i32(arg0, t0, t1);
#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_AND_I32
          tcg_gen_local_br(local_complete_label);
          gen_local_set_label(local_no_taint_label);
          tcg_gen_movi_i32(arg0, 0);
          gen_local_set_label(local_complete_label);
#endif /* BRANCH_AND_I32 */
          gen_opc_init_metadata[orig2] = 1;
#endif /* TCG_TAINT_BRANCHING */

          /* Reinsert original opcode */
          tcg_gen_and_i32(orig2, orig1, orig0);
        }
        break;

      /* Bitwise OR rules:
        Taint1 Value1 Op  Taint2 Value2  ResultingTaint
        0      0      OR  1      X       1
        1      X      OR  0      0       1
        1      X      OR  1      X       1
        ... otherwise, ResultingTaint = 0
        OR: ((NOT T1) * (NOT V1) * T2) + (T1 * (NOT T2) * (NOT V2)) + (T1 * T2)
      */
      case INDEX_op_or_i32: // Special - and_or_ty()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-1];//V1
          orig1 = gen_opparam_ptr[-2];//V2
          orig2 = gen_opparam_ptr[-3];

          /* Rewind instruction stream */
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();
          t3 = tcg_temp_new_i32();
#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_OR_I32
          local_no_taint_label = gen_new_label();
          local_complete_label = gen_new_label();

          /* Has LHS has been initialized? */
          if (!gen_opc_init_metadata[orig2]) {
            tcg_gen_or_i32(t0, arg1, arg2);
            gen_opc_init_metadata[orig2] = 1;
          } else {
            tcg_gen_or_i32(t1, arg0, arg1);
            tcg_gen_or_i32(t0, t1, arg2);
          }
          /* Skip logging/instrumentation if LHS and RHS don't have taint */
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_local_brcond_i32(TCG_COND_EQ, t0, t_zero, local_no_taint_label);
#endif /* BRANCH_OR_I32 */
#endif /* TCG_TAINT_BRANCHING */
          /* T1 -> arg1
             V1 -> gen_opparam_ptr[-2]
             T2 -> arg2
             V2 -> gen_opparam_ptr[-1] */
          if (arg1)
            tcg_gen_not_i32(t0, arg1); // NOT T1
          else
            tcg_gen_movi_i32(t0, -1);
          tcg_gen_not_i32(t1, orig1);//tcg_gen_not_i32(t1, gen_opparam_ptr[-2]); // NOT V1
          tcg_gen_and_i32(t2, t0, t1); // (NOT T1) * (NOT V1)
          if (arg2)
            tcg_gen_and_i32(t0, t2, arg2); // (NOT T1) * (NOT V1) * T2
          else
            tcg_gen_movi_i32(t0, 0);

          if (arg2)
            tcg_gen_not_i32(t1, arg2); // NOT T2
          else
            tcg_gen_movi_i32(t1, -1);
          tcg_gen_not_i32(t2, orig0);//tcg_gen_not_i32(t2, gen_opparam_ptr[-1]); // NOT V2
          tcg_gen_and_i32(t3, t1, t2); // (NOT T2) * (NOT V2)
          if (arg1)
            tcg_gen_and_i32(t1, t3, arg1); // (NOT T2) * (NOT V2) * T1
          else
            tcg_gen_movi_i32(t1, 0);

          if (arg1 && arg2)
            tcg_gen_and_i32(t2, arg1, arg2); // T1 * T2 
          else if (arg1)
            tcg_gen_mov_i32(t2, arg1);
          else if (arg2)
            tcg_gen_mov_i32(t2, arg2);
          // OR it all together
          tcg_gen_or_i32(t3, t0, t1);
          tcg_gen_or_i32(arg0, t2, t3);
#ifdef TCG_TAINT_BRANCHING
#ifdef BRANCH_OR_I32
          tcg_gen_local_br(local_complete_label);
          gen_local_set_label(local_no_taint_label);
          tcg_gen_movi_i32(arg0, 0);
          gen_local_set_label(local_complete_label);
#endif /* BRANCH_OR_I32 */
          gen_opc_init_metadata[orig2] = 1;
#endif /* TCG_TAINT_BRANCHING */
          /* Reinsert original opcode */
          tcg_gen_or_i32(orig2, orig1, orig0);
        }
        break;
#else
      /* Bytewise taint for arithmethic/shift/rotate operations (32 bit). */
      /* These all use the following pattern of shadow registers: */
      /* arg0 = arg1 op arg2.  To bitwise taint this pattern in shadow */
      /* registers, we use the following steps:
         Step 1: temp0 = arg1 or arg2
         Step 2: temp1 = 0
         Step 3: temp2 = (temp0 != temp1)
         Step 4: arg0 = ~temp2
         MemCheck: mkLazy2() for all of these */
      case INDEX_op_add_i32:
      case INDEX_op_sub_i32:
      case INDEX_op_mul_i32:
      case INDEX_op_and_i32:
      case INDEX_op_or_i32:
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          /* Determine which args are shadowed */
          if (arg1 && arg2) {
            t0 = tcg_temp_new_i32();
            tcg_gen_or_i32(t0, arg1, arg2);
          } else if (arg1) {
            t0 = tcg_temp_new_i32();
            tcg_gen_mov_i32(t0, arg1);
          } else if (arg2) {
            t0 = tcg_temp_new_i32();
            tcg_gen_mov_i32(t0, arg2);
          } else {
            tcg_gen_movi_i32(arg0, 0);
            break;
          }
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_setcond_i32(TCG_COND_NE, t2, t_zero, t0); 
          tcg_gen_neg_i32(arg0, t2);
        }
        break;
#endif /* TCG_BITWISE_TAINT */
      case INDEX_op_mulu2_i32: // Bytewise, mkLazyN()
        arg0 = find_shadow_arg(gen_opparam_ptr[-4]);
        arg1 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0 && arg1) {
          arg2 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg3 = find_shadow_arg(gen_opparam_ptr[-1]);
     
          orig0 = gen_opparam_ptr[-4];
          orig1 = gen_opparam_ptr[-3];
          orig2 = gen_opparam_ptr[-2];
          orig3 = gen_opparam_ptr[-1];
 
          if (arg2 && arg3) {
            t0 = tcg_temp_new_i32();
            tcg_gen_or_i32(t0, arg2, arg3);
          } else if (arg2) {
            t0 = tcg_temp_new_i32();
            tcg_gen_mov_i32(t0, arg2);
          } else if (arg3) {
            t0 = tcg_temp_new_i32();
            tcg_gen_mov_i32(t0, arg3);
          } else {
            tcg_gen_movi_i32(arg0, 0);
            tcg_gen_movi_i32(arg1, 0);
            break; //LOK: this is a bug - need to break it
          }
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_setcond_i32(TCG_COND_NE, t2, t0, t_zero);
#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
          gen_opc_init_metadata[orig1] = 1;
#endif /* TCG_TAINT_BRANCHING */
          tcg_gen_neg_i32(arg0, t2);
          tcg_gen_neg_i32(arg1, t2);
        }
        break;

      case INDEX_op_add2_i32: // Bytewise, mkLazyN()
      case INDEX_op_sub2_i32: // Bytewise, mkLazyN()
        arg0 = find_shadow_arg(gen_opparam_ptr[-6]); // Output low
        arg1 = find_shadow_arg(gen_opparam_ptr[-5]); // Output high
        if (arg0 && arg1) {
          arg2 = find_shadow_arg(gen_opparam_ptr[-4]); // Input1 low
          arg3 = find_shadow_arg(gen_opparam_ptr[-3]); // Input1 high
          arg4 = find_shadow_arg(gen_opparam_ptr[-2]); // Input2 low
          arg5 = find_shadow_arg(gen_opparam_ptr[-1]); // Input2 high

          orig0 = gen_opparam_ptr[-6];
          orig1 = gen_opparam_ptr[-5];
          orig2 = gen_opparam_ptr[-4];
          orig3 = gen_opparam_ptr[-3];
          orig4 = gen_opparam_ptr[-2];
          orig5 = gen_opparam_ptr[-1];

          if (!(arg2 || arg3 || arg4 || arg5)) {
#ifdef TCG_TAINT_BRANCHING
            /* LHS now initialized */
            gen_opc_init_metadata[orig0] = 1;
            gen_opc_init_metadata[orig1] = 1;
#endif /* TCG_TAINT_BRANCHING */
            tcg_gen_movi_i32(arg0, 0);
            tcg_gen_movi_i32(arg1, 0);
            break;
          }

          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();
          t3 = tcg_temp_new_i32();

          // Combine high/low taint of Input 1 into t2               
          if (arg2 && arg3)
            tcg_gen_or_i32(t2, arg2, arg3);
          else if (arg2)
            tcg_gen_mov_i32(t2, arg2);
          else if (arg3)
            tcg_gen_mov_i32(t2, arg3);
          else
            tcg_gen_movi_i32(t2, 0);

          // Combine high/low taint of Input 2 into t3
          if (arg4 && arg5)
            tcg_gen_or_i32(t3, arg4, arg5);
          else if (arg4)
            tcg_gen_mov_i32(t3, arg4);
          else if (arg5)
            tcg_gen_mov_i32(t3, arg5);
          else
            tcg_gen_movi_i32(t3, 0);

          // Determine if there is any taint
          tcg_gen_or_i32(t0, t2, t3);
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_setcond_i32(TCG_COND_NE, t2, t0, t_zero); // Reuse t2
          tcg_gen_neg_i32(arg0, t2);
          tcg_gen_neg_i32(arg1, t2);
#ifdef TCG_TAINT_BRANCHING
          /* LHS now initialized */
          gen_opc_init_metadata[orig0] = 1;
          gen_opc_init_metadata[orig1] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;

      case INDEX_op_xor_i32: // In-Place - mkUifU32
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          /* Perform an OR an arg1 and arg2 to find taint */
          if (arg1 && arg2)
            tcg_gen_or_i32(arg0, arg1, arg2);
          else if (arg1)
            tcg_gen_mov_i32(arg0, arg1);
          else if (arg2)
            tcg_gen_mov_i32(arg0, arg2);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;

#if TCG_TARGET_HAS_div_i32
      case INDEX_op_div_i32: // All-around: mkLazy2()
      case INDEX_op_divu_i32: // All-around: mkLazy2()
      case INDEX_op_rem_i32: // All-around: mkLazy2()
      case INDEX_op_remu_i32: // All-around: mkLazy2()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
        
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];
  
          if (arg1 && arg2) {
            t0 = tcg_temp_new_i32();
            tcg_gen_or_i32(t0, arg1, arg2);
          } else if (arg1) {
            t0 = tcg_temp_new_i32();
            tcg_gen_mov_i32(t0, arg1);
          } else if (arg2) {
            t0 = tcg_temp_new_i32();
            tcg_gen_mov_i32(t0, arg2);
          } else {
            tcg_gen_movi_i32(arg0, 0);
          }
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();
          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_setcond_i32(TCG_COND_NE, t2, t0, t_zero);
          tcg_gen_neg_i32(arg0, t2);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */

        }
        break;
#elif TCG_TARGET_HAS_div2_i32
      case INDEX_op_div2_i32: // All-around: mkLazy3()
      case INDEX_op_divu2_i32: // All-around: mkLazy3()
        arg0 = find_shadow_arg(gen_opparam_ptr[-5]);
        arg1 = find_shadow_arg(gen_opparam_ptr[-4]);
        if (arg0 && arg1) {
          arg2 = find_shadow_arg(gen_opparam_ptr[-3]);
          arg3 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg4 = find_shadow_arg(gen_opparam_ptr[-1]);

          orig0 = gen_opparam_ptr[-5];
          orig1 = gen_opparam_ptr[-4];
          orig2 = gen_opparam_ptr[-3];
          orig3 = gen_opparam_ptr[-2];
          orig4 = gen_opparam_ptr[-1];

          /* No shadows for any inputs */
          if (!(arg2 || arg3 || arg4))
          {
#ifdef TCG_TAINT_BRANCHING
            gen_opc_init_metadata[orig0] = 1;
            gen_opc_init_metadata[orig1] = 1;
#endif /* TCG_TAINT_BRANCHING */
            tcg_gen_movi_i32(arg0, 0);
            tcg_gen_movi_i32(arg1, 0);
            break;
          }
          t0 = tcg_temp_new_i32();
          t1 = tcg_temp_new_i32();
          t2 = tcg_temp_new_i32();

          /* Check for shadows on arg2 and arg3 */
          if (arg2 && arg3)
            tcg_gen_or_i32(t0, arg2, arg3);
          else if (arg2)
            tcg_gen_mov_i32(t0, arg2);
          else if (arg3)
            tcg_gen_mov_i32(t0, arg3);
          else
            tcg_gen_movi_i32(t0, 0);

          /* Check for shadow on arg4 */
          if (arg4)
            tcg_gen_or_i32(t2, t0, arg4);
          else
            tcg_gen_mov_i32(t2, t0);

          t_zero = tcg_temp_new_i32();
          tcg_gen_movi_i32(t_zero, 0);
          tcg_gen_setcond_i32(TCG_COND_NE, t0, t2, t_zero);
          tcg_gen_neg_i32(arg0, t0);
          tcg_gen_neg_i32(arg1, t0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
          gen_opc_init_metadata[orig1] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;
#endif /* TCG_TARGET_HAS_div*_i32 */

#if TCG_TARGET_HAS_ext8s_i32
      case INDEX_op_ext8s_i32: // MemCheck: VgT_SWiden14
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];
          if (arg1)
            tcg_gen_ext8s_i32(arg0, arg1);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;
#endif /* TCG_TARGET_HAS_ext8s_i32 */
#if TCG_TARGET_HAS_ext16s_i32
      case INDEX_op_ext16s_i32: // MemCheck: VgT_SWiden24
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];
          if (arg1)
            tcg_gen_ext16s_i32(arg0, arg1);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;
#endif /* TCG_TARGET_HAS_ext16s_i32 */
#if TCG_TARGET_HAS_ext8u_i32
      case INDEX_op_ext8u_i32: // MemCheck: VgT_ZWiden14
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];

          if (arg1)
            tcg_gen_ext8u_i32(arg0, arg1);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;
#endif /* TCG_TARGET_HAS_ext8u_i32 */
#if TCG_TARGET_HAS_ext16u_i32
      case INDEX_op_ext16u_i32: // MemCheck: VgT_ZWiden24
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];

          if (arg1)
            tcg_gen_ext16u_i32(arg0, arg1);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;
#endif /* TCG_TARGET_HAS_ext16u_i32 */
#if TCG_TARGET_HAS_bswap16_i32
      case INDEX_op_bswap16_i32: // MemCheck: UifU2
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];

          if (arg1)
            tcg_gen_bswap16_i32(arg0, arg1);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;
#endif /* TCG_TARGET_HAS_bswap16_i32 */
#if TCG_TARGET_HAS_bswap32_i32
      case INDEX_op_bswap32_i32: // MemCheck: UifU4
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];

          if (arg1)
            tcg_gen_bswap32_i32(arg0, arg1);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;
#endif /* TCG_TARGET_HAS_bswap32_i32 */
#if TCG_TARGET_HAS_not_i32
      case INDEX_op_not_i32: // MemCheck: Nothing! (Returns orig atom)
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];

          if (arg1)
            tcg_gen_mov_i32(arg0, arg1);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;
#endif /* TCG_TARGET_HAS_not_i32 */
#if TCG_TARGET_HAS_neg_i32
      case INDEX_op_neg_i32:
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-2];
          orig1 = gen_opparam_ptr[-1];

          if (arg1)
            tcg_gen_mov_i32(arg0, arg1);
          else
            tcg_gen_movi_i32(arg0, 0);
#ifdef TCG_TAINT_BRANCHING
          gen_opc_init_metadata[orig0] = 1;
#endif /* TCG_TAINT_BRANCHING */
        }
        break;

#endif /* TCG_TARGET_HAS_neg_i32 */

      case INDEX_op_movi_i64:
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
#ifdef TARGET_I386
          /* Check if the constant is a helper function for IN* opcodes */
          if ( (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_inb) ||
            (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_inw) ||
            (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_inl) )
            in_helper_func = 1;

          /* Check if the constant is a helper function for OUT* opcodes */
          else if ( (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_outb) ||
            (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_outw) ||
            (gen_opparam_ptr[-1] == (tcg_target_ulong)helper_outl) )
            out_helper_func = 1;
#endif /* TARGET_I386 */
          tcg_gen_movi_i64(arg0, 0);
        }
        break;

      case INDEX_op_mov_i64:
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
        if (arg0) {
          if (arg1)
            tcg_gen_mov_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;

      /* Arithmethic/shift/rotate operations (64 bit). */
      case INDEX_op_setcond_i64: // All-Around: UifU64() (mkLazy())
        arg0 = find_shadow_arg(gen_opparam_ptr[-4]); // Output
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-3]); // Input1
          arg2 = find_shadow_arg(gen_opparam_ptr[-2]); // Input2

          if (arg1 && arg2) {
            t0 = tcg_temp_new_i64();
            tcg_gen_or_i64(t0, arg1, arg2);
          } else if (arg1) {
            t0 = tcg_temp_new_i64();
            tcg_gen_mov_i64(t0, arg1);
          } else if (arg2) {
            t0 = tcg_temp_new_i64();
            tcg_gen_mov_i64(t0, arg2);
          } else {
            tcg_gen_mov_i64(arg0, 0);
            break;
          }

          // Determine if there is any taint
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();

          tcg_gen_movi_i64(t1, 0);
          tcg_gen_setcond_i64(TCG_COND_NE, t2, t0, t1);
          tcg_gen_neg_i64(arg0, t2);
        }
        break;

#ifdef TCG_BITWISE_TAINT
      case INDEX_op_shl_i64: // Special - scalarShift()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-1];
          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }

          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();

          if (arg2) {
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            tcg_gen_movi_i64(t0, 0);
            tcg_gen_setcond_i64(TCG_COND_NE, t1, t0, arg2);
            tcg_gen_neg_i64(t2, t1);
          } else
            tcg_gen_movi_i64(t2, 0);

          if (arg1) {
            // Perform the SHL on arg1
        	tcg_gen_shl_i64(t0, arg1, orig0);// tcg_gen_shl_i64(t0, arg1, gen_opparam_ptr[-1]);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i64(arg0, t0, t2);
          } else
            tcg_gen_mov_i64(arg0, t2);
        }
        break;

      case INDEX_op_shr_i64: // Special - scalarShift()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-1];
          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }

          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();

          if (arg2) {
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            tcg_gen_movi_i64(t0, 0);
            tcg_gen_setcond_i64(TCG_COND_NE, t1, t0, arg2);
            tcg_gen_neg_i64(t2, t1);
          } else
            tcg_gen_movi_i64(t2, 0);

          if (arg1) {
            // Perform the SHR on arg1
        	  tcg_gen_shr_i64(t0, arg1, orig0);//tcg_gen_shr_i64(t0, arg1, gen_opparam_ptr[-1]);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i64(arg0, t0, t2);
          } else
            tcg_gen_mov_i64(arg0, t2);
        }
        break;

      case INDEX_op_sar_i64: // Special - scalarShift()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-1];
          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }

          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();

          if (arg2) {
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            tcg_gen_movi_i64(t0, 0);
            tcg_gen_setcond_i64(TCG_COND_NE, t1, t0, arg2);
            tcg_gen_neg_i64(t2, t1);
          } else
            tcg_gen_movi_i64(t2, 0);

          if (arg1) {
            // Perform the SAR on arg1
            tcg_gen_sar_i64(t0, arg1, orig0);//tcg_gen_sar_i64(t0, arg1, gen_opparam_ptr[-1]);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i64(arg0, t0, t2);
          } else
            tcg_gen_mov_i64(arg0, t2);
        }
        break;

#if TCG_TARGET_HAS_rot_i64
      case INDEX_op_rotl_i64: // Special - MemCheck does lazy, but we shift
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-1];
          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }

          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();

          if (arg2) {
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            tcg_gen_movi_i64(t0, 0);
            tcg_gen_setcond_i64(TCG_COND_NE, t1, t0, arg2);
            tcg_gen_neg_i64(t2, t1);
          } else
            tcg_gen_movi_i64(t2, 0);

          if (arg1) {
            // Perform the ROTL on arg1
        	  tcg_gen_rotl_i64(t0, arg1, orig0);//tcg_gen_rotl_i64(t0, arg1, gen_opparam_ptr[-1]);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i64(arg0, t0, t2);
          } else
            tcg_gen_mov_i64(arg0, t2);
        }
        break;

      case INDEX_op_rotr_i64: // Special - MemCheck does lazy, but we shift
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-1];
          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }
          
          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();
          
          if (arg2) {
            // Check if the shift amount (arg2) is tainted.  If so, the
            // entire result will be tainted.
            tcg_gen_movi_i64(t0, 0);
            tcg_gen_setcond_i64(TCG_COND_NE, t1, t0, arg2);
            tcg_gen_neg_i64(t2, t1);
          } else
            tcg_gen_movi_i64(t2, 0);
        
          if (arg1) {
            // Perform the ROTL on arg1
        	  tcg_gen_rotr_i64(t0, arg1, orig0);//tcg_gen_rotr_i64(t0, arg1, gen_opparam_ptr[-1]);
            // OR together the taint of shifted arg1 (t0) and arg2 (t2)
            tcg_gen_or_i64(arg0, t0, t2);
          } else
            tcg_gen_mov_i64(arg0, t2);
        }
        break;
#endif /* TCG_TARGET_HAS_rot_i64 */
 // AWH - expensiveAddSub() for add_i64/or_i64 are buggy, use cheap one
      /* T0 = (T1 | T2) | ((V1_min + V2_min) ^ (V1_max + V2_max)) */
      case INDEX_op_add_i64: // Special - expensiveAddSub()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          //LOK: Changed the names of orig0 and orig 1 to orig1 and 2
          // so I don't get confused
          // Basically arg is vxx and orig is x
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          //make sure we have a copy of the values first
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          //delete the original operation
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;

          //LOK: Declared the new temporary variables that we need
          t0 = tcg_temp_new_i64(); //scratch
          t1 = tcg_temp_new_i64(); //a_min
          t2 = tcg_temp_new_i64(); //b_min
          t3 = tcg_temp_new_i64(); //a_max
          t4 = tcg_temp_new_i64(); //b_max

          /* Per the expensiveAddSub() logic:
             qaa = T1 = arg1
             qbb = T2 = arg2
             aa  = V1 = orig1
             bb  = V2 = orig2 */

          //LOK: First lets calculate a_min = aa & ~qaa
          tcg_gen_not_i64(t0, arg1); // ~qaa
          tcg_gen_and_i64(t1, orig1, t0);//t1 = aa & ~qaa
          
          //LOK: Then calculate b_min
          tcg_gen_not_i64(t0, arg2); // ~qbb
          tcg_gen_and_i64(t2, orig2, t0);//t2 = bb & ~qbb

          //LOK: Then calculate a_max = aa | qaa
          tcg_gen_or_i64(t3, orig1, arg1);//t3 = aa | qaa
          tcg_gen_or_i64(t4, orig2, arg2);//t4 = bb | qbb

          //LOK: Now that we have the mins and maxes, we need to sum them
          tcg_gen_add_i64(t0, t3, t4); // t0 = a_max + b_max
          //LOK: Note that t3 is being reused in this case
          tcg_gen_add_i64(t3, t1, t2); // t3 = a_min + b_min
          tcg_gen_xor_i64(t1, t0, t3); // t1 = ((a_min + b_min)^(a_max + b_max))
          tcg_gen_or_i64(t0, arg1, arg2); // t0 = qa | qb
          tcg_gen_or_i64(arg0, t0, t1); // arg0 = (qa | qb) | ( (a_min + b_min) ^ (a_max + b_max)

          //put the original back
          tcg_gen_add_i64(orig0, orig1, orig2);
        }
        break;

      /* T0 = (T1 | T2) | ((V1_min - V2_max) ^ (V1_max - V2_min)) */
      case INDEX_op_sub_i64: // Special - expensiveAddSub()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          //NOTE: It is important that we get the order of the operands correct
          // Right now, the assumption is
          // arg0 = arg1 - arg2
          // If there are errors - this could be the culprit

          //LOK: Changed the names of orig0 and orig 1 to orig1 and 2
          // so I don't get confused
          // Basically arg is vxx and orig is x
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          //make sure we have a copy of the values first
          orig0 = gen_opparam_ptr[-3];
          orig1 = gen_opparam_ptr[-2];
          orig2 = gen_opparam_ptr[-1];

          //delete the original operation
          gen_opparam_ptr -= 3;
          gen_opc_ptr--;
         
          //LOK: Declared the new temporary variables that we need
          t0 = tcg_temp_new_i64(); //scratch
          t1 = tcg_temp_new_i64(); //a_min
          t2 = tcg_temp_new_i64(); //b_min
          t3 = tcg_temp_new_i64(); //a_max
          t4 = tcg_temp_new_i64(); //b_max

          /* Per the expensiveAddSub() logic:
             qaa = T1 = arg1
             qbb = T2 = arg2
             aa  = V1 = orig1
             bb  = V2 = orig2 */

          //LOK: First lets calculate a_min = aa & ~qaa
          tcg_gen_not_i64(t0, arg1); // ~qaa
          tcg_gen_and_i64(t1, orig1, t0);//t1 = aa & ~qaa
          
          //LOK: Then calculate b_min
          tcg_gen_not_i64(t0, arg2); // ~qbb
          tcg_gen_and_i64(t2, orig2, t0);//t2 = bb & ~qbb

          //LOK: Then calculate a_max = aa | qaa
          tcg_gen_or_i64(t3, orig1, arg1);//t3 = aa | qaa
          tcg_gen_or_i64(t4, orig2, arg2);//t4 = bb | qbb

          //LOK: Now that we have the mins and maxes, we need to find the differences
          //NOTE: This is why the order of the operands is important
          tcg_gen_sub_i64(t0, t1, t4); // t0 = a_min - b_max
          //LOK: Note that t3 is being reused in this case
          tcg_gen_sub_i64(t4, t3, t2); // t4 = a_max - b_min
          tcg_gen_xor_i64(t1, t0, t4); // t1 = ((a_min - b_max)^(a_max - b_min))
          tcg_gen_or_i64(t0, arg1, arg2); // t0 = qa | qb
          tcg_gen_or_i64(arg0, t0, t1); // arg0 = (qa | qb) | ( (a_min - b_max) ^ (a_max - b_min)
          //put the original back
          tcg_gen_sub_i64(orig0, orig1, orig2);
        }
        break;
#if 0
      case INDEX_op_add_i64: // Up - cheap_AddSub64
      case INDEX_op_sub_i64: // Up - cheap_AddSub64
#endif
      case INDEX_op_mul_i64: // Up - mkUifU64(), mkLeft64(), mkPCastTo()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }

          t0 = tcg_temp_new_i64();
          if (arg1 && arg2)
            // mkUifU64(arg1, arg2)
            tcg_gen_or_i64(t0, arg1, arg2);
          else if (arg1)
            tcg_gen_movi_i64(t0, arg1);
          else if (arg2)
            tcg_gen_movi_i64(t0, arg2);

          // mkLeft64(t0)
          t1 = tcg_temp_new_i64();
          tcg_gen_neg_i64(t1, t0); // (-s64)
          tcg_gen_or_i64(arg0, t0, t1); // (s64 | (-s64)) -> vLo64
        }
        break;

#if TCG_TARGET_HAS_nand_i64
      case INDEX_op_nand_i64: // Special - and_or_ty()
#endif /* TCG_TARGET_HAS_nand_i64 */
      case INDEX_op_and_i64: // Special - and_or_ty()
#if TCG_TARGET_HAS_andc_i64
      case INDEX_op_andc_i64: // Special - and_or_ty()
#endif /* TCG_TARGET_HAS_andc_i64 */
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-1];
          orig1 = gen_opparam_ptr[-2];

          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }

          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();
          t3 = tcg_temp_new_i64();
          /* T1 -> arg1
             V1 -> gen_opparam_ptr[-2]
             T2 -> arg2
             V2 -> gen_opparam_ptr[-1] */
          if (arg1)
            tcg_gen_not_i64(t0, arg1); // NOT T1
          else
            tcg_gen_movi_i64(t0, -1);
          if (arg2)
        	  tcg_gen_and_i64(t1, orig1, arg2);//tcg_gen_and_i64(t1, gen_opparam_ptr[-2], arg2); // V1 * T2
          else
            tcg_gen_movi_i64(t1, 0);
          tcg_gen_and_i64(t2, t0, t1); // (NOT T1) * V1 * T2

          if (arg2)
            tcg_gen_not_i64(t0, arg2); // NOT T2
          else
            tcg_gen_movi_i64(t0, -1);
          if (arg1)
        	  tcg_gen_and_i64(t1, arg1, orig0);//tcg_gen_and_i64(t1, arg1, gen_opparam_ptr[-1]); // T1 * V2
          else
            tcg_gen_movi_i64(t1, 0);
          tcg_gen_and_i64(t3, t0, t1); // (T1 * (NOT T2) * V2)

          if (arg1 && arg2)
            tcg_gen_and_i64(t0, arg1, arg2); // T1 * T2
          else
            tcg_gen_movi_i64(t0, 0);

          // OR it all together
          tcg_gen_or_i64(t1, t2, t3);
          tcg_gen_or_i64(arg0, t0, t1);
        }
        break;

#if TCG_TARGET_HAS_nor_i64
      case INDEX_op_nor_i64: // Special - and_or_ty()
#endif /* TCG_TARGET_HAS_nor_i64 */
      case INDEX_op_or_i64: // Special - and_or_ty()
#if TCG_TARGET_HAS_orc_i64
      case INDEX_op_orc_i64: // Special - and_or_ty()
#endif /* TCG_TARGET_HAS_orc_i64 */
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
          orig0 = gen_opparam_ptr[-1];
          orig1 = gen_opparam_ptr[-2];
          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }

          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();
          t3 = tcg_temp_new_i64();
          /* T1 -> arg1
             V1 -> gen_opparam_ptr[-2]
             T2 -> arg2
             V2 -> gen_opparam_ptr[-1] */
          if (arg1)
            tcg_gen_not_i64(t0, arg1); // NOT T1
          else
            tcg_gen_movi_i64(t0, -1);
          tcg_gen_not_i64(t1, orig1);//tcg_gen_not_i64(t1, gen_opparam_ptr[-2]); // NOT V1
          tcg_gen_and_i64(t2, t0, t1); // (NOT T1) * (NOT V1)
          if (arg2)
            tcg_gen_and_i64(t0, t2, arg2); // (NOT T1) * (NOT V1) * T2
          else
            tcg_gen_movi_i64(t0, 0);

          if (arg2)
            tcg_gen_not_i64(t1, arg2); // NOT T2
          else
            tcg_gen_movi_i64(t1, -1);
          tcg_gen_not_i64(t2, orig0);//tcg_gen_not_i64(t2, gen_opparam_ptr[-1]); // NOT V2
          tcg_gen_and_i64(t3, t1, t2); // (NOT T2) * (NOT V2)
          if (arg1)
            tcg_gen_and_i64(t1, t3, arg1); // (NOT T2) * (NOT V2) * T1
          else
            tcg_gen_movi_i64(t1, 0);

          if (arg1 && arg2)
            tcg_gen_and_i64(t2, arg1, arg2); // T1 * T2 
          else if (arg1)
            tcg_gen_mov_i64(t2, arg1);
          else if (arg2)
            tcg_gen_mov_i64(t2, arg2);

          // OR it all together
          tcg_gen_or_i64(t3, t0, t1);
          tcg_gen_or_i64(arg0, t2, t3);
        }
        break;
#else
      /* These all use the following pattern of shadow registers: */
      /* arg0 = arg1 op arg2.  To bitwise taint this pattern in shadow */
      /* registers, we use the following steps:
         Step 1: temp0 = arg1 or arg2
         Step 2: temp1 = 0
         Step 3: temp2 = (temp0 != temp1)
         Step 4: arg0 = ~temp2 */
      case INDEX_op_shl_i64:
      case INDEX_op_shr_i64:
      case INDEX_op_sar_i64:
#if TCG_TARGET_HAS_rot_i64
      case INDEX_op_rotl_i64:
      case INDEX_op_rotr_i64:
#endif /* TCG_TARGET_HAS_rot_i64 */
      case INDEX_op_add_i64:
      case INDEX_op_sub_i64:
      case INDEX_op_mul_i64:
      case INDEX_op_and_i64:
      case INDEX_op_or_i64:
#if TCG_TARGET_HAS_andc_i64
      case INDEX_op_andc_i64:
#endif /* TCG_TARGET_HAS_andc_i64 */
#if TCG_TARGET_HAS_orc_i64
      case INDEX_op_orc_i64:
#endif /* TCG_TARGET_HAS_orc_i64 */
#if TCG_TARGET_HAS_nand_i64
      case INDEX_op_nand_i64:
#endif /* TCG_TARGET_HAS_nand_i64 */
#if TCG_TARGET_HAS_nor_i64
      case INDEX_op_nor_i64:
#endif /* TCG_TARGET_HAS_nor_i64 */
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          if (!arg1 && !arg2) {
            tcg_gen_movi_i32(arg0, 0);
            break;
          }
          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();
          if (arg1)
            tcg_gen_mov_i64(t0, arg1);
          else if (arg2)
            tcg_gen_mov_i64(t0, arg2);
          else
            tcg_gen_or_i64(t0, arg1, arg2);
          tcg_gen_movi_i64(t1, 0);
          tcg_gen_setcond_i64(TCG_COND_NE, t2, t0, t1);
          tcg_gen_neg_i64(arg0, t2);
        }
        break;

#endif /* TCG_BITWISE_TAINT */
#if TCG_TARGET_HAS_eqv_i64
      case INDEX_op_eqv_i64: // In-Place - mkUifU64
#endif /* TCG_TARGET_HAS_eqv_i64 */
      case INDEX_op_xor_i64: // In-Place - mkUifU64
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);

          if (arg1 && arg2)
            // Perform an OR on arg1 and arg2 to find taint
            tcg_gen_or_i64(arg0, arg1, arg2);
          else if (arg1)
            tcg_gen_mov_i64(arg0, arg1);
          else if (arg2)
            tcg_gen_mov_i64(arg0, arg2);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;

#if TCG_TARGET_HAS_div_i64
      case INDEX_op_div_i64: // All-around: mkLazy2()
      case INDEX_op_divu_i64: // All-around: mkLazy2()
      case INDEX_op_rem_i64: // All-around: mkLazy2()
      case INDEX_op_remu_i64: // All-around: mkLazy2()
        arg0 = find_shadow_arg(gen_opparam_ptr[-3]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg2 = find_shadow_arg(gen_opparam_ptr[-1]);
    
          if (!arg1 && !arg2) {
            tcg_gen_movi_i64(arg0, 0);
            break;
          }
          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();
          if (arg1)
            tcg_gen_mov_i64(t0, arg1);
          else if (arg2)
            tcg_gen_mov_i64(t0, arg2);
          else
            tcg_gen_or_i64(t0, arg1, arg2);
          tcg_gen_movi_i64(t1, 0);
          tcg_gen_setcond_i64(TCG_COND_NE, t2, t0, t1);
          tcg_gen_neg_i64(arg0, t2);
        }
        break;

#endif /* TCG_TARGET_HAS_div_i64 */
#if TCG_TARGET_HAS_div2_i64
      case INDEX_op_div2_i64: // All-around: mkLazy3()
      case INDEX_op_divu2_i64: // All-around: mkLazy3()
        arg0 = find_shadow_arg(gen_opparam_ptr[-5]);
        arg1 = find_shadow_arg(gen_opparam_ptr[-4]);
        if (arg0 && arg1) {
          arg2 = find_shadow_arg(gen_opparam_ptr[-3]);
          arg3 = find_shadow_arg(gen_opparam_ptr[-2]);
          arg4 = find_shadow_arg(gen_opparam_ptr[-1]);
          t0 = tcg_temp_new_i64();
          t1 = tcg_temp_new_i64();
          t2 = tcg_temp_new_i64();

          if (!arg2 && !arg3) {
            if (!arg4) {
              tcg_gen_movi_i64(arg0, 0);
              tcg_gen_movi_i64(arg1, 0);
              break;
            }
            tcg_gen_movi_i64(t0, 0);
          } else
            tcg_gen_or_i32(t0, arg2, arg3);
          if (arg4)
            tcg_gen_or_i32(t2, t0, arg4);
          else
            tcg_gen_mov_i32(t2, t0);

          tcg_gen_movi_i32(t1, 0);
          tcg_gen_setcond_i32(TCG_COND_NE, t0, t2, t1);
          tcg_gen_neg_i32(arg0, t0);
          tcg_gen_neg_i32(arg1, t0);
        }
        break;
#endif /* TCG_TARGET_HAS_div2_i64 */
#if TCG_TARGET_HAS_deposit_i64
      case INDEX_op_deposit_i64: // Always bitwise taint
        arg0 = find_shadow_arg(gen_opparam_ptr[-5]); // Output
        if (arg0) {
          int pos, len; // Constant parameters

          arg1 = find_shadow_arg(gen_opparam_ptr[-4]); // Input1
          arg2 = find_shadow_arg(gen_opparam_ptr[-3]); // Input2

          // Pull out the two constant parameters
          pos = gen_opparam_ptr[-2]; // Position of mask
          len = gen_opparam_ptr[-1]; // Length of mask

          // Handle special 64-bit transfer case (copy arg2 taint)
          if (len == 64) {
            if (arg2)
              tcg_gen_mov_i64(arg0, arg2);
            else
              tcg_gen_movi_i64(arg0, 0);
          // Handle special 0-bit transfer case (copy arg1 taint)
          } else if (len == 0) {
            if (arg1)
              tcg_gen_mov_i64(arg0, arg1);
            else
              tcg_gen_movi_i64(arg0, 0);
          // Handle general case
          } else {
            if (arg1 && arg2)
              tcg_gen_deposit_tl(arg0, arg1, arg2, pos, len);
            else if (arg1) {
              t0 = tcg_temp_new_i64();
              tcg_gen_movi_i64(t0, 0);
              tcg_gen_deposit_tl(arg0, arg1, t0, pos, len);
            } else if (arg2) {
              t0 = tcg_temp_new_i64();
              tcg_gen_movi_i64(t0, 0);
              tcg_gen_deposit_tl(arg0, t0, arg2, pos, len);
            }
          }
        }
        break;
#endif /* TCG_TARGET_HAS_deposit_i64 */

#if TCG_TARGET_HAS_ext8s_i64
      case INDEX_op_ext8s_i64: // MemCheck: VgT_SWiden18
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_ext8s_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_ext8s_i64 */
#if TCG_TARGET_HAS_ext16s_i64
      case INDEX_op_ext16s_i64: // MemCheck: VgT_SWiden28
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_ext16s_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_ext16s_i64 */
#if TCG_TARGET_HAS_ext32s_i64
      case INDEX_op_ext32s_i64: // MemCheck: VgT_SWiden48
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_ext32s_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_ext32s_i64 */
#if TCG_TARGET_HAS_ext8u_i64
      case INDEX_op_ext8u_i64: // MemCheck: VgT_ZWiden18
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_ext8u_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_ext8u_i64 */
#if TCG_TARGET_HAS_ext16u_i64
      case INDEX_op_ext16u_i64: // MemCheck: VgT_ZWiden28
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_ext16u_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_ext16u_i64 */
#if TCG_TARGET_HAS_ext32u_i64
      case INDEX_op_ext32u_i64: // MemCheck: VgT_ZWiden48
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_ext32u_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_ext32u_i64 */
#if TCG_TARGET_HAS_bswap16_i64
      case INDEX_op_bswap16_i64: // MemCheck: UifU2
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_bswap16_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_bswap16_i64 */
#if TCG_TARGET_HAS_bswap32_i64
      case INDEX_op_bswap32_i64: // MemCheck: UifU4
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_bswap32_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_bswap32_i64 */
#if TCG_TARGET_HAS_bswap64_i64
      case INDEX_op_bswap64_i64: // MemCheck: UifU8
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_bswap64_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_bswap64_i64 */
#if TCG_TARGET_HAS_not_i64
      case INDEX_op_not_i64: // MemCheck: nothing! Returns orig atom
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_mov_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);

        }
        break;
#endif /* TCG_TARGET_HAS_not_i64 */
#if TCG_TARGET_HAS_neg_i64
      case INDEX_op_neg_i64:
        arg0 = find_shadow_arg(gen_opparam_ptr[-2]);
        if (arg0) {
          arg1 = find_shadow_arg(gen_opparam_ptr[-1]);
          if (arg1)
            tcg_gen_mov_i64(arg0, arg1);
          else
            tcg_gen_movi_i64(arg0, 0);
        }
        break;
#endif /* TCG_TARGET_HAS_neg_i64 */

      /* QEMU-specific operations. */
      case INDEX_op_ld8u_i32:
      case INDEX_op_ld8s_i32:
      case INDEX_op_ld16u_i32:
      case INDEX_op_ld16s_i32:
      case INDEX_op_ld_i32:
      case INDEX_op_st8_i32:
      case INDEX_op_st16_i32:
      case INDEX_op_st_i32:
      case INDEX_op_ld8u_i64:
      case INDEX_op_ld8s_i64:
      case INDEX_op_ld16u_i64:
      case INDEX_op_ld16s_i64:
      case INDEX_op_ld32u_i64:
      case INDEX_op_ld32s_i64:
      case INDEX_op_ld_i64:
      case INDEX_op_st8_i64:
      case INDEX_op_st16_i64:
      case INDEX_op_st32_i64:
      case INDEX_op_st_i64:
        DUMMY_TAINT(nb_oargs, nb_args);
      /* check eip,its value and taint value*/
#ifdef LOG_TAINTED_EIP
	    arg0 = gen_opparam_ptr[-3];
        arg1 = gen_opparam_ptr[-2];
        arg2 = gen_opparam_ptr[-1];
#if defined(TARGET_I386)
        if(/*arg0 == cpu_T[0] && */arg2 == offsetof(CPUState, eip)) {
#elif defined(TARGET_ARM)
        if(arg2 == offsetof(CPUState, regs[15])) {
#elif defined(TARGET_MIPS)
        if(arg2 == (offsetof(CPUState, active_tc) + offsetof(TCState, gpr[29]))) {
#endif /* TARGET_I386/ARM */
        	TCGv shadow = shadow_arg[arg0];
        	if (shadow != 0) {
        		set_con_i32(0, arg0);
        		set_con_i32(1, shadow);
        		tcg_gen_helperN(helper_DECAF_invoke_eip_check_callback, 0, 0, TCG_CALL_DUMMY_ARG, 2, helper_arg_array);
        	}
        }
#endif
        break; /* No taint info propagated (register liveness gets these) */

      default:
        fprintf(stderr, "gen_taintcheck_insn() -> UNKNOWN %d (%s)\n", opc, tcg_op_defs[opc].name);
        fprintf(stderr, "(%s)\n", (tcg_op_defs[opc]).name);
        assert(1==0);
        break;  
    } /* End switch */
//#ifdef USE_TCG_OPTIMIZATIONS
    skip_instrumentation:;
//#endif /* USE_TCG_OPTIMIZATIONS */
  } /* End taint while loop */

  return return_lj;
#else
  return 0;
#endif /* CONFIG_TCG_TAINT */
}

int optimize_taint(int search_pc) {
int retVal;
#ifdef USE_TCG_OPTIMIZATIONS    
    if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP_OPT))) {
        qemu_log("OP partial buffer before optimization:\n");
        tcg_dump_ops(&tcg_ctx, logfile);
        qemu_log("\n");
    }

    gen_opparam_ptr =
        tcg_optimize(&tcg_ctx, gen_opc_ptr, gen_opparam_buf, tcg_op_defs);
#if 0 // AWH - Causes phantom taint in tempidx, so remove for now
    build_liveness_metadata(&tcg_ctx);
#endif // AWH
#endif
    block_count++;    
    if (unlikely(qemu_loglevel_mask(
      CPU_LOG_TB_OUT_ASM | CPU_LOG_TB_IN_ASM | 
      CPU_LOG_TB_OP | CPU_LOG_TB_OP_OPT)) )
    {
      qemu_log("------------ BEGIN BLOCK %u ---------------------\n", block_count);
    }

    if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP_OPT))) {
        qemu_log("OP partial buffer before taint instrumentation\n");
        tcg_dump_ops(&tcg_ctx, logfile);
        qemu_log("\n");
    }

    retVal = gen_taintcheck_insn(search_pc);
    if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP))) {
        qemu_log("OP after taint instrumentation\n");
        tcg_dump_ops(&tcg_ctx, logfile);
        qemu_log("\n");
    }

    return(retVal);
}
#ifdef USE_TCG_OPTIMIZATIONS
static void build_liveness_metadata(TCGContext *s)
{
    int i, op_index, nb_args, nb_iargs, nb_oargs, arg, nb_ops;
    TCGOpcode op;
    TCGArg *args;
    const TCGOpDef *def;
    uint8_t *dead_temps;
    unsigned int dead_args;

    nb_ops = gen_opc_ptr - gen_opc_buf;

    dead_temps = tcg_malloc(s->nb_temps);
    memset(dead_temps, 1, s->nb_temps);

    args = gen_opparam_ptr;
    op_index = nb_ops - 1;
    while (op_index >= 0) {
//fprintf(stderr, "op_index: %d\n", op_index);
        /* Liveness metadata (alive by default) */
        gen_old_liveness_metadata[op_index] = 1;
        op = gen_opc_buf[op_index];
        def = &tcg_op_defs[op];
        switch(op) {
        case INDEX_op_call:
            {
                int call_flags;

                nb_args = args[-1];
                args -= nb_args;
                nb_iargs = args[0] & 0xffff;
                nb_oargs = args[0] >> 16;
                args++;
                call_flags = args[nb_oargs + nb_iargs];

                /* pure functions can be removed if their result is not
                   used */
                if (call_flags & TCG_CALL_PURE) {
                    for(i = 0; i < nb_oargs; i++) {
                        arg = args[i];
                        if (!dead_temps[arg])
                            goto do_not_remove_call;
                    }
                    /* Mark the liveness metadata that this will be 
                       eliminated during liveness checks */
                    gen_old_liveness_metadata[op_index] = 0;
//fprintf(stderr, "Setting opc index: %d as dead\n", op_index);
                    //tcg_set_nop(s, gen_opc_buf + op_index,args - 1, nb_args);
                } else {
                do_not_remove_call:

                    /* output args are dead */
                    dead_args = 0;
                    for(i = 0; i < nb_oargs; i++) {
                        arg = args[i];
                        if (dead_temps[arg]) {
                            dead_args |= (1 << i);
                        }
                        dead_temps[arg] = 1;
                    }

                    if (!(call_flags & TCG_CALL_CONST)) {
                        /* globals are live (they may be used by the call) */
                        memset(dead_temps, 0, s->nb_globals);
                    }

                    /* input args are live */
                    for(i = nb_oargs; i < nb_iargs + nb_oargs; i++) {
                        arg = args[i];
                        if (arg != TCG_CALL_DUMMY_ARG) {
                            if (dead_temps[arg]) {
                                dead_args |= (1 << i);
                            }
                            dead_temps[arg] = 0;
                        }
                    }
                }
                args--;
            }
            break;
        case INDEX_op_set_label:
            args--;
            /* mark end of basic block */
            // AWH tcg_la_bb_end(s, dead_temps);
            break;
        case INDEX_op_debug_insn_start:
            args -= def->nb_args;
            break;
        case INDEX_op_nopn:
            nb_args = args[-1];
            args -= nb_args;
            break;
        case INDEX_op_discard:
            args--;
            /* mark the temporary as dead */
            dead_temps[args[0]] = 1;
            break;
        case INDEX_op_end:
            break;
            /* XXX: optimize by hardcoding common cases (e.g. triadic ops) */
        default:
            args -= def->nb_args;
            nb_iargs = def->nb_iargs;
            nb_oargs = def->nb_oargs;

            /* Test if the operation can be removed because all
               its outputs are dead. We assume that nb_oargs == 0
               implies side effects */
            if (!(def->flags & TCG_OPF_SIDE_EFFECTS) && nb_oargs != 0) {
                for(i = 0; i < nb_oargs; i++) {
                    arg = args[i];
                    if (!dead_temps[arg])
                        goto do_not_remove;
                }
                /* Mark the liveness metadata that this will be 
                   eliminated during liveness checks */
                gen_old_liveness_metadata[op_index] = 0;
//fprintf(stderr, "Setting opc index: %d as dead\n", op_index);
                //tcg_set_nop(s, gen_opc_buf + op_index, args, def->nb_args);
#ifdef CONFIG_PROFILER
//                s->del_op_count++;
#endif
            } else {
            do_not_remove:

                /* output args are dead */
                dead_args = 0;
                for(i = 0; i < nb_oargs; i++) {
                    arg = args[i];
                    if (dead_temps[arg]) {
                        dead_args |= (1 << i);
                    }
                    dead_temps[arg] = 1;
                }

                /* if end of basic block, update */
                if (def->flags & TCG_OPF_BB_END) {
                    // AWH tcg_la_bb_end(s, dead_temps);
                } else if (def->flags & TCG_OPF_CALL_CLOBBER) {
                    /* globals are live */
                    memset(dead_temps, 0, s->nb_globals);
                }

                /* input args are live */
                for(i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
                    arg = args[i];
                    if (dead_temps[arg]) {
                        dead_args |= (1 << i);
                    }
                    dead_temps[arg] = 0;
                }
            }
            break;
        }
        op_index--;
    }

    if (args != gen_opparam_buf)
        tcg_abort();
}
#endif /* USE_TCG_OPTIMIZATIONS */
#endif /* CONFIG_TCG_TAINT */
