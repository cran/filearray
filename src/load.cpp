#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
// [[Rcpp::depends(BH)]]

#include "TinyParallel.h"
#include "threadSettings.h"
#include "serialize.h"
#include "core.h"
#include "utils.h"
#include "conversion.h"
#include "load.h"
using namespace Rcpp;

int get_buffer_nelem(SEXPTYPE type){
    int buffer_bytes = get_buffer_size();
    switch(type){
    case INTSXP:
        return( buffer_bytes / sizeof(int) );
    case REALSXP:
        return( buffer_bytes / sizeof(double) );
    case RAWSXP:
        return( buffer_bytes );
    case FLTSXP:
        return( buffer_bytes / sizeof(double) );
    case LGLSXP:
        return( buffer_bytes / sizeof(int) );
    case CPLXSXP:
        return( buffer_bytes / sizeof(Rcomplex) );
    default:
        stop("Unsupported SEXP type");
    }
}

/**********************************************************
 * Subset - internal (multithread here)
 ***********************************************************/

/**
# DIPSAUS DEBUG START
# devtools::load_all()
# loadNamespace('bit64')
# 
# x <- as_filearray(1:240, dimension = c(2,3,4,10), type = "integer", partition_size = 3)
# 
# tmp <- integer(240)
# current_pos <- 80L
# FARR_subset_sequential(
# x$.filebase,
# 24L,
# bit64::as.integer64(c(3,6, 9, 10)),
# 13L,
# tmp,
# current_pos, length(x)
# ) |> print()
**/
// [[Rcpp::export]]
SEXP FARR_subset_sequential(
        const std::string& filebase, 
        const int64_t& unit_partlen, 
        SEXP cum_partsizes, 
        SEXPTYPE array_type,
        SEXP ret, 
        const int64_t from = 0, 
        const int64_t len = 1
) {
    if( TYPEOF(ret) != array_memory_sxptype(array_type) ){
        stop("Inconsistent `array_type` and return type");
    }
    if( len > Rf_xlength(ret) ){
        stop("`ret` size is too small");
    }
    if( len <= 0 ){
        return(ret);
    }
    bool swap_endian = !isLittleEndian();
    
    int file_buffer_elemsize = file_element_size(array_type);
    std::string fbase = correct_filebase(filebase);
    R_len_t nparts = Rf_length(cum_partsizes);
    int64_t* cum_part = INTEGER64(cum_partsizes);

    // We want to calculate which partitions are used, saved to part_start and part_end
    // However, each partition may contain multiple slices, hence we first decide
    // slices that's being used, stored in slice_idx1 and slice_idx2
    // in R's index format (startwith 1 and ends with to # of partitions)
    // calculate the first partition, should start from 1
    int64_t slice_idx1 = 0;
    // partition number cannot go beyond nparts + 1 (can equal)
    int64_t slice_idx2 = 0;
    int64_t tmp = 0;
    
    // printed message means get element from `from` (C index) and length of 
    // `len` across `nparts` partitions
    // Rcout << "From: " << from << " - len: " << len << " nparts: " << nparts << "\n";
    
    for(; tmp <= from; tmp+= unit_partlen, slice_idx1++){}
    
    cum_part = INTEGER64(cum_partsizes) + (nparts - 1);
    const int64_t max_slices = unit_partlen * (*cum_part);
    for(
        slice_idx2 = slice_idx1; 
        tmp < from + len && slice_idx2 < max_slices; 
        tmp+= unit_partlen, slice_idx2++
    ){}
    
    if( slice_idx2 > *cum_part ) {
        slice_idx2 = *cum_part;
    }
    
    // which slices to start and which to end
    // Rcout << "Starting from partition: " << slice_idx1 << " - ends before: " << slice_idx2 << 
    //     " (max: " << *cum_part << ")\n";
    
    // Which file partition to start: min = 0
    // unlike slice_idx1/2, part_start and part_end are index in C-style
    // That is: they starting from 0, and max is number of partitions-1
    int part_start = 0;
    int part_end = 0;
    int64_t skip_start = 0;
    // int64_t skip_end = 0;
    
    for(cum_part = INTEGER64(cum_partsizes); *cum_part < slice_idx1; cum_part++, part_start++){}
    if( part_start == 0 ){
        skip_start = from;
    } else {
        skip_start = from - (*(cum_part - 1)) * unit_partlen;
    }
    for(part_end = part_start; *cum_part < slice_idx2; cum_part++, part_end++){
        // Rcout << *cum_part << std::endl; 
    }
    // if(part_end == 0) {
    //     skip_end = unit_partlen - (from + len);
    //     // Rcout << part_start << " " << part_end << " " << *cum_part << std::endl; 
    // } else {
    //     skip_end = (*(cum_part - 1)) * unit_partlen - (from + len);
    //     // Rcout << part_start << " " << part_end << " " << *(cum_part-1) << std::endl; 
    // }
    // // This happens when buffer size is longer than array length
    // if(skip_end < 0) {
    //     skip_end = 0;
    // }
    
    // Rcpp::print(cum_partsizes);
    
    
    // Rcout << "Starting from file partition: " << (part_start + 1) <<
    //     " - ends with: " << (part_end + 1) << "\n";
    // Rcout << skip_start << "  -  " << skip_end << "\n";
    
    int64_t read_start = 0;
    int64_t read_len = 0;
    int64_t part_nelem = 0;
    cum_part = INTEGER64(cum_partsizes) + part_start;
    
    int64_t nread = 0;
    
    const boost::interprocess::mode_t mode = boost::interprocess::read_only;
    
    
    for(int part = part_start; part <= part_end; part++, cum_part++, nread += read_len){
        if( part >= nparts ){
            continue;
        }
        // get partition n_elems
        if(part == 0) {
            part_nelem = (*cum_part) * unit_partlen;
        } else {
            part_nelem = (*cum_part - *(cum_part - 1)) * unit_partlen;
        }
        // Rcout << "Starting with " << *cum_part << ", current partition contains elements: " << part_nelem << "\n";
        
        
        // skip read_start elements
        if( part == part_start ) {
            read_start = skip_start;
        } else {
            read_start = 0;
        }
        read_len = part_nelem - read_start;
        if( read_len > len - nread ) {
            read_len = len - nread;
        }
        // Rcout << "n read: " << nread << ", plan to read more: " << read_len << " from " << read_start << "\n";
        
        // Rcout << part_nelem << "--\n";
        // then read read_len elements
        // if( part == part_end ){
        //     read_len -= skip_end;
        // }
        
        // Rcout << "reading from partition: " << part << "\n";
        
        std::string part_file = fbase + std::to_string(part) + ".farr";
        
        try {
            boost::interprocess::file_mapping fm(part_file.c_str(), mode);
            boost::interprocess::mapped_region region(
                    fm, mode, 
                    FARR_HEADER_LENGTH + file_buffer_elemsize * read_start, 
                    file_buffer_elemsize * read_len);
            region.advise(boost::interprocess::mapped_region::advice_sequential);
            
            switch(array_type) {
            case REALSXP: {
                double* fbptr = static_cast<double*>(region.get_address());
                double* mbptr = REAL(ret) + nread;
                transforms_asis(fbptr, mbptr, read_len, swap_endian);
                break;
            }
            case INTSXP: {
                int* fbptr = static_cast<int*>(region.get_address());
                int* mbptr = INTEGER(ret) + nread;
                transforms_asis(fbptr, mbptr, read_len, swap_endian);
                break;
            }
            case RAWSXP: {
                Rbyte* fbptr = static_cast<Rbyte*>(region.get_address());
                Rbyte* mbptr = RAW(ret) + nread;
                transforms_asis(fbptr, mbptr, read_len, swap_endian);
                break;
            }
            case FLTSXP: {
                float* fbptr = static_cast<float*>(region.get_address());
                double* mbptr = REAL(ret) + nread;
                transforms_float(fbptr, mbptr, read_len, swap_endian);
                break;
            }
            case LGLSXP: {
                Rbyte* fbptr = static_cast<Rbyte*>(region.get_address());
                int* mbptr = LOGICAL(ret) + nread;
                transforms_logical(fbptr, mbptr, read_len, swap_endian);
                break;
            }
            case CPLXSXP: {
                double* fbptr = static_cast<double*>(region.get_address());
                Rcomplex* mbptr = COMPLEX(ret) + nread;
                transforms_complex(fbptr, mbptr, read_len, swap_endian);
                break;
            }
            default: {
                stop("Unsupported SEXP type");
            }
            }
            
        } catch (const Rcpp::LongjumpException& e) {
            std::rethrow_exception(std::current_exception());
        } catch (const boost::interprocess::interprocess_exception& e) {
            // unable to find the file, skip
        } catch (const std::exception& e) {
            std::rethrow_exception(std::current_exception());
        } catch (...) {
            throw std::runtime_error("filearray C++: Caught an unknown exception in `FARR_subset_sequential`.");
        }
        
    }
    
    return(ret);
    
}

template <typename T, typename B>
struct FARRSubsetter : public TinyParallel::Worker {
    const std::string& filebase;
    const List& sch;
    const T na; 
    const R_xlen_t& retlen;
    T* ret_ptr;
    void (*transform)(const B*, T*, const bool&);
    const int elem_size;
    
    SEXP idx1;
    SEXP idx1range;
    R_xlen_t idx1len;
    int64_t idx1_start;
    int64_t idx1_end;
    List idx2s;
    int64_t block_size;
    IntegerVector partitions;
    IntegerVector idx2lens;
    
    bool skip_all;
    bool swap_endian;
    boost::interprocess::mode_t mode;
    int has_error;
    std::string error_msg;
    
    bool use_mmap;
    std::vector<B*> buf_ptrs;
    
    FARRSubsetter(
        const std::string& filebase, 
        const List& sch,
        T* ret_ptr, const T na, const R_xlen_t& retlen,
        void (*transform)(const B*, T*, const bool&),
        const bool& use_mmap
    ): filebase(filebase), sch(sch), na(na), retlen(retlen),
    ret_ptr(ret_ptr), transform(transform), elem_size(sizeof(B)){
        
        this->idx1 = sch["idx1"];
        this->idx1range = sch["idx1range"];
        this->idx1len = Rf_xlength(idx1);
        int64_t* idx1rangeptr = (int64_t*) REAL(idx1range);
        this->idx1_start = *idx1rangeptr;
        this->idx1_end = *(idx1rangeptr + 1);
        
        this->idx2s = sch["idx2s"];
        this->block_size = (int64_t) (sch["block_size"]);
        this->partitions = sch["partitions"];
        this->idx2lens = sch["idx2lens"];
        
        
        if( idx1_start == NA_INTEGER64 || idx1_end < 0 || idx1_start < 0 ){
            // idx1 are all NAs, no need to subset, return NA

            T* retptr = ret_ptr;
            for(R_xlen_t jj = 0; jj < retlen; jj++){
                *retptr++ = na;
            }
            this->skip_all = true;
        } else {
            this->skip_all = false;
        }
        
        this->mode = boost::interprocess::read_only;
        this->swap_endian = !isLittleEndian();
        
        this->has_error = -1;
        this->error_msg = "";
        // char* buffer[nbuffers];
        
        // R_xlen_t niter = partitions.length();
        // int ncores = getThreads();
        // if(ncores > niter){
        //     ncores = niter;
        // }
        this->use_mmap = use_mmap;
        
    }
    
    void operator_mmap(std::size_t begin, std::size_t end) {
        
        for(R_xlen_t ii = begin; ii < end; ii++){
            
            int part = partitions[ii];
            int64_t skips = 0;
            if(ii > 0){
                skips = idx2lens[ii - 1];
            }
            int64_t idx2len = idx2lens[ii] - skips;
            
            // TODO: change
            T* retptr = ret_ptr + skips * idx1len;
            for(R_xlen_t jj = 0; jj < idx2len * idx1len; jj++, retptr++ ){
                *retptr = na;
            }
            
            // TODO: change
            retptr = ret_ptr + skips * idx1len;
            
            SEXP idx2 = idx2s[ii];
            int64_t idx2_start = NA_INTEGER64, idx2_end = -1;
            int64_t* ptr2 = (int64_t*) REAL(idx1); 
            for(ptr2 = (int64_t*) REAL(idx2); idx2len > 0; idx2len--, ptr2++ ){
                if( *ptr2 == NA_INTEGER64 ){
                    continue;
                }
                if( *ptr2 < idx2_start || idx2_start == NA_INTEGER64 ){
                    idx2_start = *ptr2;
                }
                if( idx2_end < *ptr2 ){
                    idx2_end = *ptr2;
                }
            }
            
            if( idx2_start == NA_INTEGER64 || idx2_end < 0 || idx2_start < 0 ){
                // This is NA partition, no need to subset
                continue;
            }
            
            // const int idx2_sorted = kinda_sorted(idx2, idx2_start, 1);
            std::string file = filebase + std::to_string(part) + ".farr";
            
            try{
                boost::interprocess::file_mapping fm(file.c_str(), mode);
                boost::interprocess::mapped_region region(
                        fm, mode, 
                        FARR_HEADER_LENGTH + elem_size * (
                                idx2_start * block_size + idx1_start
                        ), 
                        elem_size * (
                                idx1_end - idx1_start + 1 +
                                    block_size * (idx2_end - idx2_start)
                        ));
                // region.advise(boost::interprocess::mapped_region::advice_sequential);
                const B* mmap_ptr = static_cast<const B*>(region.get_address());
                // const int64_t content_size = region.get_size() / elem_size;
                
                
                // prepare for all the pointers, local variables
                int64_t* idx2ptr = INTEGER64(idx2);
                R_xlen_t idx2len = Rf_xlength(idx2);
                R_xlen_t ii_idx2 = 0;
                int64_t start_idx = idx1_start;
                
                int64_t* idx1ptr = INTEGER64(idx1);
                
                R_xlen_t jj = 0;
                T* retptr2 = retptr;
                
                for(; ii_idx2 < idx2len; ii_idx2++, idx2ptr++) {
                    
                    if ( *idx2ptr == NA_INTEGER64 ){
                        continue;
                    }
                    
                    // Rcout << block << "\n";
                    
                    // read current block!
                    retptr2 = retptr + ii_idx2 * idx1len;
                    start_idx = block_size * (*idx2ptr - idx2_start);
                    
                    idx1ptr = INTEGER64(idx1);
                    start_idx -= idx1_start;
                    
                    for(jj = 0; jj < idx1len; jj++, idx1ptr++, retptr2++) {
                        if(*idx1ptr != NA_INTEGER64){
                            transform(mmap_ptr + (start_idx + *idx1ptr), retptr2, swap_endian);
                        }
                        
                    }
                    
                }
            } catch(...) {
                // Debug use
                // err = part;
            }
            
        }
    }
    
    void operator_fread(std::size_t begin, std::size_t end) {
        std::size_t ncores = buf_ptrs.size();
        for(R_xlen_t ii = begin; ii < end; ii++){
            
            int part = partitions[ii];
            int64_t skips = 0;
            if(ii > 0){
                skips = idx2lens[ii - 1];
            }
            int64_t idx2len = idx2lens[ii] - skips;
            
            // TODO: change
            T* retptr = ret_ptr + skips * idx1len;
            for(R_xlen_t jj = 0; jj < idx2len * idx1len; jj++, retptr++ ){
                *retptr = na;
            }
            
            // TODO: change
            retptr = ret_ptr + skips * idx1len;
            
            SEXP idx2 = idx2s[ii];
            int64_t idx2_start = NA_INTEGER64, idx2_end = -1;
            int64_t* ptr2 = (int64_t*) REAL(idx1); 
            for(ptr2 = (int64_t*) REAL(idx2); idx2len > 0; idx2len--, ptr2++ ){
                if( *ptr2 == NA_INTEGER64 ){
                    continue;
                }
                if( *ptr2 < idx2_start || idx2_start == NA_INTEGER64 ){
                    idx2_start = *ptr2;
                }
                if( idx2_end < *ptr2 ){
                    idx2_end = *ptr2;
                }
            }
            
            if( idx2_start == NA_INTEGER64 || idx2_end < 0 || idx2_start < 0 ){
                // This is NA partition, no need to subset
                continue;
            }
            
            // const int idx2_sorted = kinda_sorted(idx2, idx2_start, 1);
            std::string file = filebase + std::to_string(part) + ".farr";
            
            FILE* conn = fopen(file.c_str(), "rb");
            if( conn == NULL ){
                continue;
            }
            
            try{
                B* buf_ptr = buf_ptrs[ii % ncores];
                
                // prepare for all the pointers, local variables
                int64_t* idx2ptr = INTEGER64(idx2);
                R_xlen_t idx2len = Rf_xlength(idx2);
                R_xlen_t ii_idx2 = 0;
                // int64_t start_idx = idx1_start;
                
                int64_t* idx1ptr = INTEGER64(idx1);
                
                R_xlen_t jj = 0;
                T* retptr2 = retptr;
                
                for(; ii_idx2 < idx2len; ii_idx2++, idx2ptr++) {
                    
                    if ( *idx2ptr == NA_INTEGER64 ){
                        continue;
                    }
                    
                    // Rcout << block << "\n";
                    
                    // read current block!
                    retptr2 = retptr + ii_idx2 * idx1len;
                    idx1ptr = INTEGER64(idx1);
                    
                    fseek(conn, FARR_HEADER_LENGTH + elem_size * (
                            *idx2ptr * block_size + idx1_start
                    ), SEEK_SET);
                    lendian_fread(buf_ptr, elem_size, idx1_end - idx1_start + 1, conn);
                    
                    for(jj = 0; jj < idx1len; jj++, idx1ptr++, retptr2++) {
                        if(*idx1ptr != NA_INTEGER64){
                            transform(buf_ptr + (*idx1ptr - idx1_start), retptr2, false);
                        }
                        
                    }
                    
                }
            } catch(...) {
                // Debug use
                // err = part;
            }
            
            if( conn != NULL ){
                fclose(conn);
            }
            
        }
    }
    
    void operator()(std::size_t begin, std::size_t end) {
        if( this->skip_all ) { return; }
        if( this->use_mmap ) {
            this->operator_mmap(begin, end);
        } else {
            this->operator_fread(begin, end);
        }
    }
    
    void load() {
        if( this->skip_all ) { return; }
        
        int ncores = (int)(this->buf_ptrs.size());
        
        if( !this->use_mmap ) {
            
            if( ncores == 0 ) {
                this->use_mmap = true;
            }
        }
        
        if( this->use_mmap ) {
            parallelFor(0, partitions.length(), *this);
        } else {
            // TODO: calculate grain size
            parallelFor(0, partitions.length(), *this, 1, ncores);
        }
        
        if( has_error >= 0 ){
            stop("Error while trying to read partition " + 
                std::to_string(has_error + 1) + 
                ". Reason: " + error_msg);
        }
    }
    
};


template <typename T, typename B>
void FARR_subset_mmap(
        const std::string& filebase, 
        const List& sch,
        T* ret_ptr, const T na, const R_xlen_t& retlen,
        void (*transform)(const B*, T*, const bool&)
) {
    FARRSubsetter<T, B> subsetter(filebase, sch, ret_ptr, na, retlen, transform, false);
    subsetter.load();
}

template <typename T, typename B>
void FARR_subset_fread(
        const std::string& filebase, 
        const List& sch,
        T* ret_ptr, const T na, const R_xlen_t& retlen,
        const std::vector<B*> buf_ptrs,
        void (*transform)(const B*, T*, const bool&)
){
    FARRSubsetter<T, B> subsetter(filebase, sch, ret_ptr, na, retlen, transform, true);
    subsetter.buf_ptrs = buf_ptrs;
    subsetter.load();
}

SEXP FARR_subset(const std::string& filebase, 
                 const List& sch,
                 const SEXPTYPE type,
                 SEXP ret){
    std::string fbase = correct_filebase(filebase);
    
    SEXP idx1 = sch["idx1"];
    R_xlen_t idx1len = Rf_xlength(idx1);
    // IntegerVector partitions = sch["partitions"];
    // IntegerVector idx2lens = sch["idx2lens"];
    
    // R_xlen_t niter = partitions.length();
    
    // R_xlen_t retlen = idx1len * idx2lens[niter - 1];
    // 
    // SEXPTYPE ret_type = array_memory_sxptype(type);
    // SEXP ret = PROTECT(Rf_allocVector(ret_type, retlen));
    
    R_xlen_t retlen = Rf_xlength(ret);
    SEXP result_dim = sch["result_dim"];
    Rf_setAttrib(ret, R_DimSymbol, result_dim);
    
    
    int ncores = getThreads();
    if( ncores < 1 ){
        stop("Thread number and buffer pool size must be positive.");
    }
    
    bool use_mmap = false;
    // Allocate buffers
    SEXPTYPE buffer_type = file_buffer_sxptype(type);
    SEXP idx1range = sch["idx1range"];
    int64_t* idx1rangeptr = INTEGER64(idx1range);
    int64_t idx1_start = *idx1rangeptr, idx1_end = *(idx1rangeptr + 1);
    int64_t buffer_nelems = idx1_end - idx1_start + 1;
    if( idx1_end < 0 || idx1_start == NA_INTEGER64 || idx1_start < 0 ){
        use_mmap = true;
    } else if ( buffer_nelems > 2 * idx1len ){ // TODO: test the ratio
        use_mmap = true;
    }
    
    
    if(use_mmap){
        switch(type){
        case INTSXP: {
            FARR_subset_mmap<int, int>(
                    fbase, sch, INTEGER(ret), NA_INTEGER, retlen,
                    &transform_asis);
            break;
        }
        case REALSXP: {
            FARR_subset_mmap<double, double>(
                    fbase, sch, REAL(ret), NA_REAL, retlen,
                    &transform_asis);
            break;
        }
        case FLTSXP: {
            FARR_subset_mmap<double, float>(
                    fbase, sch, REAL(ret), NA_REAL, retlen,
                    &transform_float);
            break;
        }
        case RAWSXP: {
            FARR_subset_mmap<Rbyte, Rbyte>(
                    fbase, sch, RAW(ret), NA_RBYTE, retlen,
                    &transform_asis);
            break;
        }
        case LGLSXP: {
            FARR_subset_mmap<int, Rbyte>(
                    fbase, sch, LOGICAL(ret), NA_LOGICAL, retlen,
                    &transform_logical);
            break;
        }
        case CPLXSXP: {
            na_cplx_dbl();
            Rcomplex na_cplx;
            na_cplx.i = NA_REAL;
            na_cplx.r = NA_REAL;
            FARR_subset_mmap<Rcomplex, double>(
                    fbase, sch, COMPLEX(ret), na_cplx, retlen,
                    &transform_complex);
            break;
        }
        default:
            stop("Unsupported SEXP type");
        }
        
    } else {
        SEXP buf = PROTECT(Rf_allocVector(buffer_type, ncores * buffer_nelems));
        switch(type){
        case INTSXP: {
            std::vector<int*> buf_ptrs(ncores);
            for(int i = 0; i < ncores; i++){
                buf_ptrs[i] = INTEGER(buf) + i * buffer_nelems;
            }
            FARR_subset_fread<int, int>(
                    fbase, sch, INTEGER(ret), NA_INTEGER, retlen,
                    buf_ptrs,
                    &transform_asis);
            break;
        }
        case REALSXP: {
            std::vector<double*> buf_ptrs(ncores);
            for(int i = 0; i < ncores; i++){
                buf_ptrs[i] = REAL(buf) + i * buffer_nelems;
            }
            FARR_subset_fread<double, double>(
                    fbase, sch, REAL(ret), NA_REAL, retlen,
                    buf_ptrs,
                    &transform_asis);
            break;
        }
        case FLTSXP: {
            std::vector<float*> buf_ptrs(ncores);
            for(int i = 0; i < ncores; i++){
                buf_ptrs[i] = FLOAT(buf) + i * buffer_nelems;
            }
            FARR_subset_fread<double, float>(
                    fbase, sch, REAL(ret), NA_REAL, retlen,
                    buf_ptrs,
                    &transform_float);
            break;
        }
        case RAWSXP: {
            std::vector<Rbyte*> buf_ptrs(ncores);
            for(int i = 0; i < ncores; i++){
                buf_ptrs[i] = RAW(buf) + i * buffer_nelems;
            }
            FARR_subset_fread<Rbyte, Rbyte>(
                    fbase, sch, RAW(ret), NA_RBYTE, retlen,
                    buf_ptrs,
                    &transform_asis);
            break;
        }
        case LGLSXP: {
            std::vector<Rbyte*> buf_ptrs(ncores);
            for(int i = 0; i < ncores; i++){
                buf_ptrs[i] = RAW(buf) + i * buffer_nelems;
            }
            FARR_subset_fread<int, Rbyte>(
                    fbase, sch, LOGICAL(ret), NA_LOGICAL, retlen,
                    buf_ptrs,
                    &transform_logical);
            break;
        }
        case CPLXSXP: {
            na_cplx_dbl();
            Rcomplex na_cplx;
            na_cplx.i = NA_REAL;
            na_cplx.r = NA_REAL;
            std::vector<double*> buf_ptrs(ncores);
            for(int i = 0; i < ncores; i++){
                buf_ptrs[i] = REAL(buf) + i * buffer_nelems;
            }
            FARR_subset_fread<Rcomplex, double>(
                    fbase, sch, COMPLEX(ret), na_cplx, retlen,
                    buf_ptrs,
                    &transform_complex);
            break;
        }
        default:
            UNPROTECT(1);
            stop("Unsupported SEXP type");
        }
        
        UNPROTECT(1);
    }
    
    
    return(ret);
}

// [[Rcpp::export]]
SEXP FARR_subset2(
        const std::string& filebase,
        const SEXP listOrEnv,
        const SEXP reshape = R_NilValue,
        const bool drop = false,
        const bool use_dimnames = true,
        size_t thread_buffer = 0,
        int split_dim = 0,
        const int strict = 1
) {
    const std::string fbase = correct_filebase(filebase);
    List meta = FARR_meta(fbase);
    const int elem_size = meta["elem_size"];
    const SEXPTYPE sexp_type = meta["sexp_type"];
    SEXP dim = meta["dimension"]; // double
    SEXP cum_part_size = meta["cumsum_part_sizes"];
    
    R_len_t ndims = Rf_length(dim);
    
    int current_bufsize = get_buffer_size();
    if( thread_buffer <= 0 ){
        thread_buffer = current_bufsize;
    }
    
    // calculate split_dim
    if( split_dim == NA_INTEGER || split_dim == 0 ){
        split_dim = guess_splitdim(dim, elem_size, thread_buffer);
    } else if (split_dim < 1 || split_dim > ndims-1 ){
        stop("Incorrect `split_dim`: must be an integer from 1 to ndims-1 ");
    }
    set_buffer(dim, elem_size, thread_buffer, split_dim);
    
    // get dimnames
    SEXP dnames = R_NilValue;
    SEXP sliceIdx = PROTECT(locationList(listOrEnv, dim, 1));
    
    if( use_dimnames ){
        dnames = meta["dimnames"];
        if( TYPEOF(dnames) == VECSXP && Rf_length(dnames) == ndims ){
            subset_dimnames(dnames, sliceIdx);
        }
    }
    
    // schedule indices
    List sch = schedule(sliceIdx, dim, cum_part_size, split_dim, strict);
    
    // allocate for returns
    int64_t retlen = *INTEGER64(sch["result_length"]);
    // const SEXP idx1 = sch["idx1"];
    // const IntegerVector idx2lens = sch["idx2lens"];
    // R_xlen_t idx1len = Rf_xlength(idx1);
    // R_xlen_t retlen = idx1len * idx2lens[Rf_length(cum_part_size) - 1];
    // 
    SEXPTYPE ret_type = array_memory_sxptype(sexp_type);
    SEXP res = PROTECT(Rf_allocVector(ret_type, retlen));
    
    FARR_subset(fbase, sch, sexp_type, res);
    if( dnames != R_NilValue ){
        Rf_setAttrib(res, R_DimNamesSymbol, dnames);
    }
    reshape_or_drop(res, reshape, drop);
    // R_gc();
    
    set_buffer_size(current_bufsize);
    
    UNPROTECT(2);
    return(res);
}


/*** R
# devtools::load_all()
loadNamespace('bit64')

x <- as_filearray(1:240, dimension = c(2,3,4,10), type = "integer", partition_size = 3)

tmp <- integer(240)
current_pos <- 0L
FARR_subset_sequential(
    x$.filebase,
    24L,
    bit64::as.integer64(c(3,6, 9, 10)),
    13L,
    tmp,
    current_pos, length(x)
)

# 
# set.seed(1); file <- tempfile(); unlink(file, recursive = TRUE)
# x <- filearray_create(file, 3:5, partition_size = 1, type = "double")
# x[] <- 1:60
# 
# filearray_threads(1)
# FARR_subset2(filebase = x$.filebase, listOrEnv = list(), 
#             reshape = NULL, drop = FALSE)
# 
# 
# # set_buffer_size(31)
# 
# # unlink(file)
# set.seed(1)
# basefile <- normalizePath(tempdir(check = TRUE), mustWork = TRUE)
# file <- file.path(basefile, '0.farr')
# unlink(file)
# write_partition(file, 1, c(3,4,1), as.double(1:12), "double")
# file <- file.path(basefile, '1.farr')
# unlink(file)
# write_partition(file, 1, c(3,4,2), as.double(13:36), "double")
# file <- file.path(basefile, '2.farr')
# unlink(file)
# write_partition(file, 1, c(3,4,2), as.double(37:60), "double")
# # 
# # 
# # # fid = file(file, "w+b"); write_header(fid, 1, c(400, 100, 500, 5), "double", 8L); close(fid)
# # write_partition(file, 1, c(400, 100, 500, 5), as.double(1:1e8), "double")
# # 
# # idx1 <- bit64::as.integer64(0:39999)
# # idx2 <- bit64::as.integer64(sample(0:2499))
# # 
# # system.time({
# #     c_subset(file, 40000, idx1, idx2)
# # }, gcFirst = TRUE)
# # # unlink(file)
# 
# 
# # re <- structure(realToInt64(c(1L,2L,NA_integer_), 1, 3), class = 'integer64')
# # re
# # 
# # a <- bit64::as.integer64.double(c(1,2,NA))
# # class(a) <- NULL; a
# 
# # loc2idx(locationList(list(),c(3,2), 1), c(3,2))
# # loc2idx(list(),c(3,2))
# # loc2idx(list(c(1,2,NA,3,4), 1:10), c(4,2), strict = 0)
# # (function(...){
# #     loc2idx(environment(), c(3,2))
# # })(c(1,2,NA), )
# 
# # re <- bit64::as.integer64(rep(0.0, 12))
# # x <- bit64::as.integer64(as.double(1:3))
# # addCycle(x, re, 4)
# 
# basefile <- paste0(basefile, '/')
# 
# a <- FARR_subset(filebase = basefile, type = 14L, 
#            listOrEnv = list(c(1,2,3,3,2,1,NA,2,2), c(2,4,1,3, NA, 1), c(1:5,5:1,NA,3)),  
#            dim = c(3:5),
#            cum_part_sizes = cumsum(c(1,2,2)), 
#            split_dim = 2)
# 
# b <- array(as.double(1:60), 3:5)[c(1,2,3,3,2,1,NA,2,2), c(2,4,1,3, NA, 1), c(1:5,5:1,NA,3)]
# identical(a, b)
# testthat::expect_equal(a, b)
*/
