#include "merge_runner.h"

#define TREE_LIMIT  2000

int run_merge(shared_options_data_t *shared_options_data, merge_options_data_t *options_data) {
    if (options_data->num_files == 1) {
        LOG_INFO("Just one VCF file specified, no need to merge");
        return 0;
    }
    
    list_t *read_list[options_data->num_files];
    memset(read_list, 0, options_data->num_files * sizeof(list_t*));
    list_t *output_list = (list_t*) malloc (sizeof(list_t));
    list_init("output", shared_options_data->num_threads, shared_options_data->max_batches * shared_options_data->batch_lines, output_list);
    
    int ret_code = 0;
    double start, stop, total;
    vcf_file_t *files[options_data->num_files];
    memset(files, 0, options_data->num_files * sizeof(vcf_file_t*));
    
    // Initialize variables related to the different files
    for (int i = 0; i < options_data->num_files; i++) {
        files[i] = vcf_open(options_data->input_files[i], shared_options_data->max_batches);
        if (!files[i]) {
            LOG_FATAL_F("VCF file %s does not exist!\n", options_data->input_files[i]);
        }
        
        read_list[i] = (list_t*) malloc(sizeof(list_t));
        list_init("text", 1, shared_options_data->max_batches, read_list[i]);
    }
    
    ret_code = create_directory(shared_options_data->output_directory);
    if (ret_code != 0 && errno != EEXIST) {
        LOG_FATAL_F("Can't create output directory: %s\n", shared_options_data->output_directory);
    }
    
#pragma omp parallel sections private(start, stop, total)
    {
#pragma omp section
        {
            LOG_DEBUG_F("Thread %d reads the VCF file\n", omp_get_thread_num());
            // Reading
            start = omp_get_wtime();

            ret_code = vcf_multiread_batches(read_list, shared_options_data->batch_lines, files, options_data->num_files);

            stop = omp_get_wtime();
            total = stop - start;

            if (ret_code) {
                LOG_ERROR_F("Error %d while reading VCF files\n", ret_code);
            }

            LOG_INFO_F("[%dR] Time elapsed = %f s\n", omp_get_thread_num(), total);
            LOG_INFO_F("[%dR] Time elapsed = %e ms\n", omp_get_thread_num(), total*1000);
            
//             for (int i = 0; i < options_data->num_files; i++) {
//                 list_decr_writers(read_list[i]);
//             }
        }
        
#pragma omp section
        {
            // Enable nested parallelism and set the number of threads the user has chosen
            omp_set_nested(1);
            omp_set_num_threads(shared_options_data->num_threads);
            
            LOG_DEBUG_F("Thread %d processes data\n", omp_get_thread_num());
            
            int num_eof_found = 0;
            int eof_found[options_data->num_files];
            memset(eof_found, 0, options_data->num_files * sizeof(int));
            
            list_item_t *items[options_data->num_files];
            memset(items, 0, options_data->num_files * sizeof(list_item_t*));
            char *texts[options_data->num_files];
            memset(texts, 0, options_data->num_files * sizeof(char*));
            cp_hashtable *positions_read = cp_hashtable_create(shared_options_data->batch_lines * options_data->num_files * 2, 
                                                               cp_hash_long,
                                                               cp_hash_compare_long
                                                              );
            long max_position_merged = LONG_MAX;
            char *max_chromosome_merged = NULL;
            int chromosome_change = 0;
            
            int num_chromosomes;
            char **chromosome_order = get_chromosome_order(shared_options_data->host_url, shared_options_data->species, 
                                                           shared_options_data->version, &num_chromosomes);
            
            start = omp_get_wtime();

            while (num_eof_found < options_data->num_files) {
                /* Process:
                 * - N threads getting batches of VCF records and inserting them in a data structure. The common minimum 
                 * position of each group of batches will also be stored.
                 * - If the data structure reaches certain size or the end of a chromosome, merge positions prior to the 
                 * last minimum registered.
                 */
                
#pragma omp parallel num_threads(shared_options_data->num_threads) firstprivate(items, texts)
                {
#pragma omp critical
                {
                // Getting text elements in a critical region guarantees that each thread gets variants in positions in the same range
                for (int i = 0; i < options_data->num_files; i++) {
                    if (eof_found[i]) {
                        continue;
                    }
                    
                    items[i] = list_remove_item(read_list[i]);
                    if (items[i] == NULL || !strcmp(items[i]->data_p, "")) {
                        LOG_INFO_F("[%d] EOF found in file %s\n", omp_get_thread_num(), options_data->input_files[i]);
                        eof_found[i] = 1;
                        num_eof_found++;
                        
                        if(items[i] != NULL && !strcmp(items[i]->data_p, "")) {
                            free(items[i]->data_p);
                            list_item_free(items[i]);
                            LOG_DEBUG_F("[%d] Text batch freed\n", omp_get_thread_num());
                        } else {
                            LOG_DEBUG_F("[%d] No need to free text batch\n", omp_get_thread_num());
                        }
                        
                        continue;
                    }
                    
                    assert(items[i]->data_p != NULL);
                    texts[i] = items[i]->data_p;
                    
//                     printf("[%d] text batch from file %d\n", omp_get_thread_num(), i);
//                     printf("[%d] contents = '%s'\n", texts[i]);
                }
                }
                
                for (int i = 0; i < options_data->num_files; i++) {
                    if (eof_found[i]) {
                        continue;
                    }
                    
                    char *text_begin = texts[i];
                    char *text_end = text_begin + strlen(text_begin);
                    assert(text_end != NULL);
                    
                    // Get VCF batches from text batches
                    vcf_reader_status *status = vcf_reader_status_new(shared_options_data->batch_lines, 1);
                    ret_code = run_vcf_parser(text_begin, text_end, shared_options_data->batch_lines, files[i], status);
                    
                    vcf_batch_t *batch = fetch_vcf_batch(files[i]);
                    
//                     printf("[%d] vcf batch from file %d\n", omp_get_thread_num(), i);
                    
                    // Insert records into hashtable
                    for (int j = 0; j < batch->records->size; j++) {
                        vcf_record_t *record = array_list_get(j, batch->records);
                        cp_list *records_in_position = cp_hashtable_get(positions_read, &(record->position));
                        if (records_in_position != NULL) {
                            cp_list_append(records_in_position, record);
                        } else {
                            records_in_position = cp_list_create();
                            cp_list_append(records_in_position, record);
                            long *position = (long*) malloc (sizeof(long));
                            *position = record->position;
                            cp_hashtable_put(positions_read, position, records_in_position);
                        }
                    }
                    
                    // Update minimum position being a maximum of these batches
                    vcf_record_t *current_record = (vcf_record_t*) array_list_get(batch->records->size - 1, batch->records);
                    char *current_chromosome = current_record->chromosome;
                    long unsigned int current_position = current_record->position;
                    
#pragma omp critical
                    {
                    if (max_chromosome_merged == NULL) {
                        // Max merged chrom:position not set, assign without any other consideration
                        max_chromosome_merged = strdup(current_chromosome);
                        max_position_merged = current_position;
                    } else {
                        int chrom_comparison = compare_chromosomes(current_chromosome, max_chromosome_merged, chromosome_order, num_chromosomes);
                        int position_comparison = compare_positions(&current_position, &max_position_merged);
                        
                        // Max merged chrom:position is posterior to the last one in this batch 
                        if (chrom_comparison <= 0 && position_comparison < 0) {
                            max_chromosome_merged = strdup(current_chromosome);
                            max_position_merged = current_position;
                        }  /*else if (chrom_comparison > 0) {
                            chromosome_change = 1;
                            LOG_DEBUG_F("Chromosome change (%s -> %s)\n", max_chromosome_merged, current_chromosome);
                        }*/
                    }
                    
                    // Free batch and its contents
//                     vcf_batch_free_shallow(batch);
//                     list_item_free(batch_item);
//                     free(texts[i]);
                    free(batch);
                    list_item_free(items[i]);
                    free(status);
                    }
                }
                
                LOG_DEBUG_F("Last position to potentially merge = %s:%ld\n", max_chromosome_merged, max_position_merged);
                
                // If the data structure reaches certain size or the end of a chromosome, 
                // merge positions prior to the last minimum registered TODO
//                 if (num_eof_found < options_data->num_files && (cp_hashtable_count(positions_read) > TREE_LIMIT || chromosome_change)) {
                if (num_eof_found < options_data->num_files && cp_hashtable_count(positions_read) > TREE_LIMIT) {
                    long **keys = (long**) cp_hashtable_get_keys(positions_read);
                    int num_keys = cp_hashtable_count(positions_read);
                    
//                     for (int i = 0; i < num_keys; i++) {
//                         printf("position #%d = %ld\n", i, *(keys[i]));
//                     }
                    
                    // TODO launch merge
//                     LOG_INFO_F("***** Merging until position = %s:%ld\n", max_chromosome_merged, max_position_merged);
                    
                    // Free records in the table
                    for (int k = 0; k < num_keys; k++) {
                        cp_list *records_in_position = cp_hashtable_get(positions_read, keys[k]);
                        assert(records_in_position != NULL);
                        cp_list_iterator *iterator = cp_list_create_iterator(records_in_position, COLLECTION_LOCK_WRITE);
                        vcf_record_t *record = NULL;
                        // Free records in a node, and whose positions are prior to the last chromosome:position to merge
                        while ((record = cp_list_iterator_curr(iterator)) != NULL) {
                            /* Each node is identified by its position, but several chromosomes can coexist at the same time.
                             * Merged records must be positioned:
                             * - In a previous chromosome
                             * - In the same chromosome, in a previous position
                             */
                            if (compare_chromosomes(record->chromosome, max_chromosome_merged, 
                                                    chromosome_order, num_chromosomes) < 0 ||
                                (compare_chromosomes(record->chromosome, max_chromosome_merged, 
                                                     chromosome_order, num_chromosomes) == 0 &&
                                compare_positions(&(record->position), &max_position_merged) <= 0)) {
                                LOG_DEBUG_F("Freeing record %s:%ld\n", record->chromosome, record->position);
                                cp_list_iterator_remove(iterator);
                                vcf_record_free(record);
                            }
                            
                            cp_list_iterator_next(iterator);
                        }
                        cp_list_iterator_destroy(iterator);
                        // Free empty nodes (lists of records in the same position)
                        if (cp_list_item_count(records_in_position) == 0) {
                            cp_list_destroy(cp_hashtable_remove(positions_read, keys[k]));
                        }
                    }
                }
                
                // When reaching EOF for all files, merge the remaining entries
                if (num_eof_found == options_data->num_files && cp_hashtable_count(positions_read) > 0) {
                    long **keys = (long**) cp_hashtable_get_keys(positions_read);
                    int num_keys = cp_hashtable_count(positions_read);
                    
                    // TODO launch merge
                    LOG_DEBUG("Last merging\n");
                    
                    // Free remaining records in the table
                    for (int k = 0; k < num_keys; k++) {
                        cp_list *records_in_position = cp_hashtable_get(positions_read, keys[k]);
                        assert(records_in_position != NULL);
                        cp_list_iterator *iterator = cp_list_create_iterator(records_in_position, COLLECTION_LOCK_WRITE);
                        vcf_record_t *record = NULL;
                        // Free records in a node, and whose positions are prior to the last chromosome:position to merge
                        while ((record = cp_list_iterator_curr(iterator)) != NULL) {
                            LOG_DEBUG_F("Freeing record %s:%ld\n", record->chromosome, record->position);
                            // Each node is identified by its position, but several chromosomes can coexist at the same time
                            cp_list_iterator_remove(iterator);
                            vcf_record_free(record);
                            cp_list_iterator_next(iterator);
                        }
                        cp_list_iterator_destroy(iterator);
                        // Free empty nodes (lists of records in the same position)
                        if (cp_list_item_count(records_in_position) == 0) {
                            cp_list_destroy(cp_hashtable_remove(positions_read, keys[k]));
                        }
                    }
                }
                
                // Set variables ready for next iteration of the algorithm
                free(max_chromosome_merged);
                max_chromosome_merged = NULL;
                max_position_merged = LONG_MAX;
                chromosome_change = 0;
                }
            }
            
//             printf("Remaining entries = %ld\n", cp_hashtable_count(positions_read));
//             
//             long **keys = (long**) cp_hashtable_get_keys(positions_read);
//             int num_keys = cp_hashtable_count(positions_read);
//             
//             for (int i = 0; i < num_keys; i++) {
//                 printf("position #%d = %ld\n", i, *(keys[i]));
//             }

            free(positions_read);
            
            stop = omp_get_wtime();

            total = stop - start;

            LOG_INFO_F("[%d] Time elapsed = %f s\n", omp_get_thread_num(), total);
            LOG_INFO_F("[%d] Time elapsed = %e ms\n", omp_get_thread_num(), total*1000);
// 
//             // Decrease list writers count
//             for (i = 0; i < shared_options_data->num_threads; i++) {
//                 list_decr_writers(output_list);
//             }
        }
        
#pragma omp section
        {
//             LOG_DEBUG_F("Thread %d writes the output\n", omp_get_thread_num());
//     
//             start = omp_get_wtime();
// 
//             // Create file streams for results
//             int dirname_len = strlen(shared_options_data->output_directory);
//             
//             list_item_t* item = NULL;
//             merge_result_t *merge;
//             FILE *merge_fd = NULL;
//             char merge_filename[1024];
//             char input_filename[256];
//             get_filename_from_path(shared_options_data->vcf_filename, input_filename);
//             
//             while ((item = list_remove_item(output_list)) != NULL) {
//                 merge = item->data_p;
//                 
//                 memset(merge_filename, 0, 1024 * sizeof(char));
//                 sprintf(merge_filename, "%s/%s_%s", shared_options_data->output_directory, merge->merge_name, input_filename);
// //                 sprintf(merge_filename, "%s/%s.vcf", shared_options_data->output_directory, merge->merge_name, shared_options_data->vcf_filename);
//                 
// //                 printf("Split filename = '%s'\n", merge_filename);
//                 
//                 merge_fd = cp_hashtable_get(output_files, merge->merge_name);
//                 if (!merge_fd) {
//                     // TODO If its the first line to write into the file, create file and include the header
//                     merge_fd = fopen(merge_filename, "w");
//                     cp_hashtable_put(output_files, merge->merge_name, merge_fd);
//                     
//                     vcf_write_to_file(file, merge_fd);
//                 }
//                 
//                 // TODO write line into the file
//                 write_record(merge->record, merge_fd);
//                 vcf_record_free(merge->record);
//             }
//             
//             stop = omp_get_wtime();
// 
//             total = stop - start;
// 
//             LOG_INFO_F("[%dW] Time elapsed = %f s\n", omp_get_thread_num(), total);
//             LOG_INFO_F("[%dW] Time elapsed = %e ms\n", omp_get_thread_num(), total*1000);
        }
    }

    // TODO Free variables related to the different files
    for (int i = 0; i < options_data->num_files; i++) {
        if(files[i]) { vcf_close(files[i]); }
        if(read_list[i]) { free(read_list[i]); }
    }
    free(output_list);
    
    return ret_code;
}
