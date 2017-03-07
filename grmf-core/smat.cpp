
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cmath>

#include "smat.h"
#include "zlib_util.h"


#define MALLOC(type, size) (type*)malloc(sizeof(type)*(size))

void smat_t::clear_space() {
	if(mem_alloc_by_me) {
		if(read_from_binary) 
			free(binary_buf);
		else {
			if(val)free(val); if(val_t)free(val_t);
			if(row_ptr)free(row_ptr);if(row_idx)free(row_idx); 
			if(col_ptr)free(col_ptr);if(col_idx)free(col_idx);
		}
	}
	read_from_binary = false;
	mem_alloc_by_me = false;
}

smat_t smat_t::transpose(){
	smat_t mt;
	mt.cols = rows; mt.rows = cols; mt.nnz = nnz;
	mt.val = val_t; mt.val_t = val;
	mt.col_ptr = row_ptr; mt.row_ptr = col_ptr;
	mt.col_idx = row_idx; mt.row_idx = col_idx;
	mt.max_col_nnz=max_row_nnz; mt.max_row_nnz=max_col_nnz;
	return mt;
}

smat_t smat_t::row_subset(const std::vector<int> &subset) {
	return row_subset(&subset[0], (int)subset.size());
}
smat_t smat_t::row_subset(const int *subset, int subset_size) {
	smat_subset_iterator_t it(*this, subset, subset_size);
	smat_t sub_smat;
	sub_smat.load_from_iterator(subset_size, cols, it.nnz, &it);
	return sub_smat;
}

double smat_t::get_global_mean() const {
	double sum=0;
	for(long i=0;i<nnz;++i) sum+=val[i];
	return sum/(double)nnz;
}

void smat_t::remove_bias(double bias){
	if(bias) {
		for(long i=0;i<nnz;++i) val[i]-=bias;
		for(long i=0;i<nnz;++i) val_t[i]-=bias;
	}
}

void smat_t::Xv(const double *v, double *Xv) {
	for(size_t i = 0; i < rows; ++i) {
		Xv[i] = 0;
		for(long idx = row_ptr[i]; idx < row_ptr[i+1]; ++idx) 
			Xv[i] += val_t[idx] * v[col_idx[idx]];
	}
}

void smat_t::XTu(const double *u, double *XTu) {
	for(size_t i = 0; i < cols; ++i) {
		XTu[i] = 0;
		for(long idx = col_ptr[i]; idx < col_ptr[i+1]; ++idx) 
			XTu[i] += val[idx] * u[row_idx[idx]];
	}
}

// Comparator for sorting rates into row/column comopression storage
class SparseComp {
	public:
		const unsigned *row_idx;
		const unsigned *col_idx;
		SparseComp(const unsigned *row_idx_, const unsigned *col_idx_, bool isRCS_=true) {
			row_idx = (isRCS_)? row_idx_: col_idx_;
			col_idx = (isRCS_)? col_idx_: row_idx_;
		}
		bool operator()(size_t x, size_t y) const {
			return  (row_idx[x] < row_idx[y]) || ((row_idx[x] == row_idx[y]) && (col_idx[x]<= col_idx[y]));
		}
};

void smat_t::load_from_iterator(long _rows, long _cols, long _nnz, entry_iterator_t* entry_it){
	rows =_rows,cols=_cols,nnz=_nnz;
	mem_alloc_by_me = true;
	val = MALLOC(double, nnz); val_t = MALLOC(double, nnz);
	row_idx = MALLOC(unsigned, nnz); col_idx = MALLOC(unsigned, nnz);  
	//row_idx = MALLOC(unsigned long, nnz); col_idx = MALLOC(unsigned long, nnz); // switch to this for matlab
	row_ptr = MALLOC(long, rows+1); col_ptr = MALLOC(long, cols+1);
	memset(row_ptr,0,sizeof(long)*(rows+1));
	memset(col_ptr,0,sizeof(long)*(cols+1));

	// a trick here to utilize the space the have been allocated 
	std::vector<size_t> perm(_nnz);
	unsigned *tmp_row_idx = col_idx;
	unsigned *tmp_col_idx = row_idx;
	double *tmp_val = val;
	for(long idx = 0; idx < _nnz; idx++){
		rate_t rate = entry_it->next();
		row_ptr[rate.i+1]++;
		col_ptr[rate.j+1]++;
		tmp_row_idx[idx] = rate.i; 
		tmp_col_idx[idx] = rate.j;
		tmp_val[idx] = rate.v;
		perm[idx] = idx;
	}
	// sort entries into row-majored ordering
	sort(perm.begin(), perm.end(), SparseComp(tmp_row_idx, tmp_col_idx, true));
	// Generate CRS format
	for(long idx = 0; idx < _nnz; idx++) {
		val_t[idx] = tmp_val[perm[idx]];
		col_idx[idx] = tmp_col_idx[perm[idx]];
	}

	// Calculate nnz for each row and col
	max_row_nnz = max_col_nnz = 0;
	for(size_t r=1; r<=rows; ++r) {
		max_row_nnz = std::max(max_row_nnz, row_ptr[r]);
		row_ptr[r] += row_ptr[r-1];
	}
	for(size_t c=1; c<=cols; ++c) {
		max_col_nnz = std::max(max_col_nnz, col_ptr[c]);
		col_ptr[c] += col_ptr[c-1];
	}
	// Transpose CRS into CCS matrix
	for(unsigned r=0; r<rows; ++r){
		for(long i = row_ptr[r]; i < row_ptr[r+1]; ++i){
			long c = col_idx[i];
			row_idx[col_ptr[c]] = r; 
			val[col_ptr[c]++] = val_t[i];
		}
	}
	for(long c=cols; c>0; --c) col_ptr[c] = col_ptr[c-1];
	col_ptr[0] = 0;
}

void smat_t::load(long _rows, long _cols, long _nnz, const char* filename){
	file_iterator_t entry_it(_nnz, filename);
	load_from_iterator(_rows, _cols, _nnz, &entry_it);
}

void smat_t::save_binary_to_file(const char *filename) {
	size_t byteswritten = 0;
	FILE *fp = fopen(filename, "wb");
	if(fp == NULL) { fprintf(stderr,"Cannot open %s to write.",filename); }
	fseek(fp, HeaderSize, SEEK_SET);
	zlib_writer compresser;
	byteswritten += compresser.write(col_ptr, sizeof(long), cols+1, fp);
	byteswritten += compresser.write(row_idx, sizeof(unsigned), nnz, fp);
	byteswritten += compresser.write(val, sizeof(double), nnz, fp, Z_FINISH);
	/*
	   byteswritten += compresser.write(row_ptr, sizeof(long), rows+1, fp);
	   byteswritten += compresser.write(col_idx, sizeof(unsigned), nnz, fp);
	   byteswritten += compresser.write(val_t, sizeof(double), nnz, fp, Z_FINISH);
	   */
	// Write Header
	rewind(fp);
	fwrite(&rows,sizeof(size_t), 1, fp);
	fwrite(&cols,sizeof(size_t), 1, fp);
	fwrite(&nnz,sizeof(size_t), 1, fp);
	fwrite(&byteswritten,sizeof(long), 1, fp);
	fclose(fp);
}


void smat_t::load_from_binary(const char *filename) {
	unsigned char *compressedbuf;
	size_t compressedbuf_len;
	FILE *fp = fopen(filename, "rb");
	if(fp == NULL) { fprintf(stderr,"Cannot open %s to read.",filename); return;}
	read_from_binary = true;
	mem_alloc_by_me = true;

	// Read header and allocate memory
	fread(&rows,sizeof(size_t), 1, fp);
	fread(&cols,sizeof(size_t), 1, fp);
	fread(&nnz,sizeof(size_t), 1, fp);
	fread(&compressedbuf_len,sizeof(size_t), 1, fp);
	binary_buf_len = sizeof(size_t)*(rows+1+cols+1)+sizeof(unsigned)*nnz*2+sizeof(double)*nnz*2;
	compressedbuf = MALLOC(unsigned char, compressedbuf_len);
	binary_buf = MALLOC(unsigned char, binary_buf_len);

	// Read the binary file and decompress it
	size_t decompressed_buf_len = sizeof(size_t)*(cols+1)+sizeof(unsigned)*nnz+sizeof(double)*nnz;
	fread(compressedbuf, sizeof(unsigned char), compressedbuf_len, fp);
	zlib_decompress(binary_buf, &decompressed_buf_len, compressedbuf, compressedbuf_len);
	free(compressedbuf);
	fclose(fp);

	// Parse the file
	size_t offset = 0;
	col_ptr = (long*) (binary_buf + offset); offset += sizeof(long)*(cols+1);
	row_idx = (unsigned*) (binary_buf + offset); offset += sizeof(unsigned)*nnz;
	val = (double*) (binary_buf + offset); offset += sizeof(double)*nnz;
	row_ptr = (long*) (binary_buf + offset); offset += sizeof(long)*(rows+1);
	col_idx = (unsigned*) (binary_buf + offset); offset += sizeof(unsigned)*nnz;
	val_t = (double*) (binary_buf + offset); offset += sizeof(double)*nnz;

	// Transpose CCS into CRS matrix
	memset(row_ptr, 0, sizeof(long)*(rows+1));
	for(long idx = 0; idx < nnz; idx++)
		row_ptr[row_idx[idx]+1]++;
	for(unsigned r = 1; r <= rows; r++)
		row_ptr[r] += row_ptr[r-1];
	for(unsigned c = 0; c < cols; c++) {
		for(long idx = col_ptr[c]; idx < col_ptr[c+1]; idx++) {
			long r = row_idx[idx];
			col_idx[row_ptr[r]] = c;
			val_t[row_ptr[r]++] = val[idx];
		}
	}
	for(long r = rows; r > 0; --r) 
		row_ptr[r] = row_ptr[r-1];
	row_ptr[0] = 0;

	max_col_nnz = 0;
	for(unsigned c=1; c<=cols; ++c) 
		max_col_nnz = std::max(max_col_nnz, 
				nnz_of_col(c));
	max_row_nnz = 0;
	for(unsigned r=1; r<=rows; ++r)
		max_row_nnz = std::max(max_row_nnz, nnz_of_row(r));
}


file_iterator_t::file_iterator_t(size_t nnz_, const char* filename){
	nnz = nnz_;
	fp = fopen(filename,"r");
}
rate_t file_iterator_t::next() {
	int i = 1, j = 1;
	double v = 0;
	if (nnz > 0) {
		fscanf(fp, "%d %d %lf", &i, &j, &v);
		--nnz;
	} else {
		fprintf(stderr,"Error: no more entry to iterate !!\n");
	}
	return rate_t(i-1,j-1,v);
}

smat_iterator_t::smat_iterator_t(const smat_t& M, int major) {
	nnz = M.nnz;
	col_idx = (major == ROWMAJOR)? M.col_idx: M.row_idx;
	row_ptr = (major == ROWMAJOR)? M.row_ptr: M.col_ptr;
	val_t = (major == ROWMAJOR)? M.val_t: M.val;
	rows = (major==ROWMAJOR)? M.rows: M.cols;
	cols = (major==ROWMAJOR)? M.cols: M.rows;
	cur_idx = cur_row = 0;
}
rate_t smat_iterator_t::next() {
	while (cur_idx >= row_ptr[cur_row+1]) 
		cur_row++;
	if (nnz > 0) 
		nnz--;
	else 
		fprintf(stderr,"Error: no more entry to iterate !!\n");
	rate_t ret(cur_row, col_idx[cur_idx], val_t[cur_idx]);
	cur_idx++;
	return ret;
}

smat_subset_iterator_t::smat_subset_iterator_t(const smat_t& M, const int *subset, int size, int major) {
	col_idx = (major == ROWMAJOR)? M.col_idx: M.row_idx;
	row_ptr = (major == ROWMAJOR)? M.row_ptr: M.col_ptr;
	val_t = (major == ROWMAJOR)? M.val_t: M.val;
	rows = (major==ROWMAJOR)? M.rows: M.cols;
	cols = (major==ROWMAJOR)? M.cols: M.rows;
	this->subset.resize(size);
	nnz = 0;
	for(int i = 0; i < size; i++) {
		int idx = subset[i];
		this->subset[i] = idx;
		nnz += (major == ROWMAJOR)? M.nnz_of_row(idx): M.nnz_of_col(idx);
	}
	sort(this->subset.begin(), this->subset.end());
	cur_row = 0;
	cur_idx = row_ptr[this->subset[cur_row]];
}

rate_t smat_subset_iterator_t::next() {
	while (cur_idx >= row_ptr[subset[cur_row]+1]) {
		cur_row++;
		cur_idx = row_ptr[subset[cur_row]];
	}
	if (nnz > 0) 
		nnz--;
	else 
		fprintf(stderr,"Error: no more entry to iterate !!\n");
	rate_t ret(cur_row, col_idx[cur_idx], val_t[cur_idx]);
	//printf("%d %d\n", cur_row, col_idx[cur_idx]);
	cur_idx++;
	return ret;
}


/*
   H = X*W 
   X is an m*n
   W is an n*k, row-majored array
   H is an m*k, row-majored array
   */
void smat_x_dmat(const smat_t &X, const double* W, const size_t k, double *H)
{
	size_t m = X.rows;
#pragma omp parallel for schedule(dynamic,50) shared(X,H,W)
	for(size_t i = 0; i < m; i++) {
		double *Hi = &H[k*i];
		memset(Hi,0,sizeof(double)*k);
		for(long idx = X.row_ptr[i]; idx < X.row_ptr[i+1]; idx++) {
			const double Xij = X.val_t[idx];
			const double *Wj = &W[X.col_idx[idx]*k];
			for(unsigned t = 0; t < k; t++)
				Hi[t] += Xij*Wj[t];
		}
	}
}

/*
   H = a*X*W + b*H
   X is an m*n
   W is an n*k, row-majored array
   H is an m*k, row-majored array
   */
void smat_x_dmat(const double a, const smat_t &X, const double* W, const size_t k, const double b, double *H) {
	size_t m = X.rows;
	if(a != 1.0) {
#pragma omp parallel for schedule(dynamic,50) shared(X,H,W)
		for(size_t i = 0; i < m; i++) {
			double *Hi = &H[k*i];
			if(b != 1.0)
				for(unsigned t = 0; t < k; t++) Hi[t] *= b;
			for(long idx = X.row_ptr[i]; idx < X.row_ptr[i+1]; idx++) {
				const double Xij = X.val_t[idx];
				const double *Wj = &W[X.col_idx[idx]*k];
				for(unsigned t = 0; t < k; t++)
					Hi[t] += a*Xij*Wj[t];
			}
		}
	} else  {
#pragma omp parallel for schedule(dynamic,50) shared(X,H,W)
		for(size_t i = 0; i < m; i++) {
			double *Hi = &H[k*i];
			if(b != 1.0)
				for(unsigned t = 0; t < k; t++) Hi[t] *= b;
			for(long idx = X.row_ptr[i]; idx < X.row_ptr[i+1]; idx++) {
				const double Xij = X.val_t[idx];
				const double *Wj = &W[X.col_idx[idx]*k];
				for(unsigned t = 0; t < k; t++)
					Hi[t] += Xij*Wj[t];
			}
		}
	}
}
/*
   H = a*X*W + H0
   X is an m*n
   W is an n*k, row-majored array
   H is an m*k, row-majored array
   */
void smat_x_dmat(const double a, const smat_t &X, const double* W, const size_t k, const double *H0, double *H)
{
	size_t m = X.rows;
#pragma omp parallel for schedule(dynamic,50) shared(X,H,W)
	for(size_t i = 0; i < m; i++) {
		double *Hi = &H[k*i];
		if(H != H0)
			memcpy(Hi, &H0[k*i], sizeof(double)*k);
		for(long idx = X.row_ptr[i]; idx < X.row_ptr[i+1]; idx++) {
			const double Xij = X.val_t[idx];
			const double *Wj = &W[X.col_idx[idx]*k];
			for(unsigned t = 0; t < k; t++)
				Hi[t] += a*Xij*Wj[t];
		}
	}
}

/*
	trace(W^T X H) 
	X is an m*n, sparse matrix
	W is an m*k, row-majored array
	H is an n*k, row-major
 
*/
double trace_dmat_smat_dmat(const double *W, const smat_t &X, const double *H, const size_t k) 
{
	/*
	double *XH  = MALLOC(double, X.rows*k);
	smat_x_dmat(X, H, k, XH);
	double sum = 0;
	for(size_t i =0; i < X.rows*k; i++)
		sum += W[i]*XH[i];
	free(XH);
	return sum;
	*/

	size_t m = X.rows;
	double ret = 0;
#pragma omp parallel for schedule(dynamic,50) shared(X,H,W) reduction(+:ret)
	for(size_t i = 0; i < m; i++) {
		const double *Wi = &W[k*i];
		for(long idx = X.row_ptr[i]; idx < X.row_ptr[i+1]; idx++) {
			const double *Hj = &H[X.col_idx[idx]*k];
			double tmp=0;
			for(unsigned t = 0; t < k; ++t)
				tmp += Wi[t]*Hj[t];
			ret += X.val_t[idx]*tmp;
		}
	}
	return ret;
}

