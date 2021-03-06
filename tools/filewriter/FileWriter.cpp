/*
 * FileWriter.cpp
 *
 */
#include <assert.h>

#include "FileWriter.h"
#include <hdf5_hl.h>
#include "Frame.h"
#include <stdio.h>

namespace filewriter
{

const std::string FileWriter::CONFIG_PROCESS        = "process";
const std::string FileWriter::CONFIG_PROCESS_NUMBER = "number";
const std::string FileWriter::CONFIG_PROCESS_RANK   = "rank";

const std::string FileWriter::CONFIG_FILE           = "file";
const std::string FileWriter::CONFIG_FILE_NAME      = "name";
const std::string FileWriter::CONFIG_FILE_PATH      = "path";

const std::string FileWriter::CONFIG_DATASET        = "dataset";
const std::string FileWriter::CONFIG_DATASET_CMD    = "cmd";
const std::string FileWriter::CONFIG_DATASET_NAME   = "name";
const std::string FileWriter::CONFIG_DATASET_TYPE   = "datatype";
const std::string FileWriter::CONFIG_DATASET_DIMS   = "dims";
const std::string FileWriter::CONFIG_DATASET_CHUNKS = "chunks";

const std::string FileWriter::CONFIG_FRAMES         = "frames";
const std::string FileWriter::CONFIG_MASTER_DATASET = "master";
const std::string FileWriter::CONFIG_WRITE          = "write";

herr_t hdf5_error_cb(unsigned n, const H5E_error2_t *err_desc, void* client_data)
{
  FileWriter *fwPtr = (FileWriter *)client_data;
  fwPtr->hdfErrorHandler(n, err_desc);
  return 0;
}

/**
 * Create a FileWriterPlugin with default values.
 * File path is set to default of current directory, and the
 * filename is set to a default.  The framesToWrite_ member
 * variable is set to a default of 3 (TODO: Change this).
 *
 * The writer plugin is also configured to be a single
 * process writer (no other expected writers) with an offset
 * of 0.
 */
FileWriter::FileWriter() :
  writing_(false),
  masterFrame_(""),
  framesToWrite_(3),
  framesWritten_(0),
  filePath_("./"),
  fileName_("test_file.h5"),
  concurrent_processes_(1),
  concurrent_rank_(0),
  hdf5_fileid_(0),
  hdf5ErrorFlag_(false),
  start_frame_offset_(0)
{
    this->logger_ = Logger::getLogger("FW.FileWriter");
    this->logger_->setLevel(Level::getTrace());
    LOG4CXX_TRACE(logger_, "FileWriter constructor.");

    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
    H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, hdf5_error_cb, this);
    //H5Eset_auto2(H5E_DEFAULT, my_hdf5_error_handler, NULL);

    this->hdf5_fileid_ = 0;
    this->start_frame_offset_ = 0;
}

/**
 * Destructor.
 */
FileWriter::~FileWriter()
{
    if (this->hdf5_fileid_ > 0) {
        LOG4CXX_TRACE(logger_, "destructor closing file");
        H5Fclose(this->hdf5_fileid_);
        this->hdf5_fileid_ = 0;
    }
}

/**
 * Create the HDF5 ready for writing datasets.
 * Currently the file is created with the following:
 * Chunk boundary alignment is set to 4MB.
 * Using the latest library format
 * Created with SWMR access
 * chunk_align parameter not currently used
 *
 * \param[in] filename - Full file name of the file to create.
 * \param[in] chunk_align - Not currently used.
 */
void FileWriter::createFile(std::string filename, size_t chunk_align)
{
    hid_t fapl; // File access property list
    hid_t fcpl;

    // Create file access property list
    fapl = H5Pcreate(H5P_FILE_ACCESS);
    assert(fapl >= 0);

    assert(H5Pset_fclose_degree(fapl, H5F_CLOSE_STRONG) >= 0);

    // Set chunk boundary alignment to 4MB
    assert( H5Pset_alignment( fapl, 65536, 4*1024*1024 ) >= 0);

    // Set to use the latest library format
    assert(H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) >= 0);

    // Create file creation property list
    if ((fcpl = H5Pcreate(H5P_FILE_CREATE)) < 0)
    assert(fcpl >= 0);

    // Creating the file with SWMR write access
    LOG4CXX_INFO(logger_, "Creating file: " << filename);
    unsigned int flags = H5F_ACC_TRUNC;
    this->hdf5_fileid_ = H5Fcreate(filename.c_str(), flags, fcpl, fapl);
    if (this->hdf5_fileid_ < 0){
      // Close file access property list
      assert(H5Pclose(fapl) >= 0);
      // Now throw a runtime error to explain that the file could not be created
      std::stringstream err;
      err << "Could not create file " << filename;
      throw std::runtime_error(err.str().c_str());
    }
    // Close file access property list
    assert(H5Pclose(fapl) >= 0);
}

/**
 * Write a frame to the file.
 *
 * \param[in] frame - Reference to the frame.
 */
void FileWriter::writeFrame(const Frame& frame) {
    herr_t status;
    hsize_t frame_no = frame.get_frame_number();

    HDF5Dataset_t& dset = this->get_hdf5_dataset(frame.get_dataset_name());

    hsize_t frame_offset = 0;
    frame_offset = this->getFrameOffset(frame_no);
    this->extend_dataset(dset, frame_offset + 1);

    LOG4CXX_DEBUG(logger_, "Writing frame offset=" << frame_no  << " (" << frame_offset << ")"
    		             << " dset=" << frame.get_dataset_name());

    // Set the offset
    std::vector<hsize_t>offset(dset.dataset_dimensions.size());
    offset[0] = frame_offset;

    uint32_t filter_mask = 0x0;
    status = H5DOwrite_chunk(dset.datasetid, H5P_DEFAULT,
                             filter_mask, &offset.front(),
                             frame.get_data_size(), frame.get_data());
    assert(status >= 0);
}

/**
 * Write horizontal subframes direct to dataset chunk.
 *
 * \param[in] frame - Reference to a frame object containing the subframe.
 */
void FileWriter::writeSubFrames(const Frame& frame) {
    herr_t status;
    uint32_t filter_mask = 0x0;
    hsize_t frame_no = frame.get_frame_number();

    HDF5Dataset_t& dset = this->get_hdf5_dataset(frame.get_dataset_name());

    hsize_t frame_offset = 0;
    frame_offset = this->getFrameOffset(frame_no);

    this->extend_dataset(dset, frame_offset + 1);

    LOG4CXX_DEBUG(logger_, "Writing frame=" << frame_no << " (" << frame_offset << ")"
    					<< " dset=" << frame.get_dataset_name());

    // Set the offset
    std::vector<hsize_t>offset(dset.dataset_dimensions.size(), 0);
    offset[0] = frame_offset;

    for (int i = 0; i < frame.get_parameter("subframe_count"); i++)
    {
      offset[2] = i * frame.get_dimensions("subframe")[1]; // For P2M: subframe is 704 pixels
        LOG4CXX_DEBUG(logger_, "    offset=" << offset[0]
                  << "," << offset[1] << "," << offset[2]);

        LOG4CXX_DEBUG(logger_, "    subframe_size=" << frame.get_parameter("subframe_size"));

        status = H5DOwrite_chunk(dset.datasetid, H5P_DEFAULT,
                                 filter_mask, &offset.front(),
                                 frame.get_parameter("subframe_size"),
                                 (static_cast<const char*>(frame.get_data())+(i*frame.get_parameter("subframe_size"))));
        assert(status >= 0);
    }
}

/**
 * Create a HDF5 dataset from the DatasetDefinition.
 *
 * \param[in] definition - Reference to the DatasetDefinition.
 */
void FileWriter::createDataset(const FileWriter::DatasetDefinition& definition)
{
    // Handles all at the top so we can remember to close them
    hid_t dataspace = 0;
    hid_t prop = 0;
    hid_t dapl = 0;
    herr_t status;
    hid_t dtype = pixelToHdfType(definition.pixel);

    std::vector<hsize_t> frame_dims = definition.frame_dimensions;

    // Dataset dims: {1, <image size Y>, <image size X>}
    std::vector<hsize_t> dset_dims(1,1);
    dset_dims.insert(dset_dims.end(), frame_dims.begin(), frame_dims.end());

    // If chunking has not been defined it defaults to a single full frame
    std::vector<hsize_t> chunk_dims(1, 1);
    if (definition.chunks.size() != dset_dims.size()) {
    	chunk_dims = dset_dims;
    } else {
    	chunk_dims = definition.chunks;
    }

    std::vector<hsize_t> max_dims = dset_dims;
    max_dims[0] = H5S_UNLIMITED;

    /* Create the dataspace with the given dimensions - and max dimensions */
    dataspace = H5Screate_simple(dset_dims.size(), &dset_dims.front(), &max_dims.front());
    assert(dataspace >= 0);

    /* Enable chunking  */
    LOG4CXX_DEBUG(logger_, "Chunking=" << chunk_dims[0] << ","
                       << chunk_dims[1] << ","
                       << chunk_dims[2]);
    prop = H5Pcreate(H5P_DATASET_CREATE);
    status = H5Pset_chunk(prop, dset_dims.size(), &chunk_dims.front());
    assert(status >= 0);

    char fill_value[8] = {0,0,0,0,0,0,0,0};
    status = H5Pset_fill_value(prop, dtype, fill_value);
    assert(status >= 0);

    dapl = H5Pcreate(H5P_DATASET_ACCESS);

    /* Create dataset  */
    LOG4CXX_DEBUG(logger_, "Creating dataset: " << definition.name);
    FileWriter::HDF5Dataset_t dset;
    dset.datasetid = H5Dcreate2(this->hdf5_fileid_, definition.name.c_str(),
                                        dtype, dataspace,
                                        H5P_DEFAULT, prop, dapl);
    if (dset.datasetid < 0){
      // Unable to create the dataset, clean up resources
      assert( H5Pclose(prop) >= 0);
      assert( H5Pclose(dapl) >= 0);
      assert( H5Sclose(dataspace) >= 0);
      // Now throw a runtime error to notify that the dataset could not be created
      throw std::runtime_error("Unable to create the dataset");
    }
    dset.dataset_dimensions = dset_dims;
    dset.dataset_offsets = std::vector<hsize_t>(3);
    this->hdf5_datasets_[definition.name] = dset;

    LOG4CXX_DEBUG(logger_, "Closing intermediate open HDF objects");
    assert( H5Pclose(prop) >= 0);
    assert( H5Pclose(dapl) >= 0);
    assert( H5Sclose(dataspace) >= 0);
}

/**
 * Close the currently open HDF5 file.
 */
void FileWriter::closeFile() {
    LOG4CXX_TRACE(logger_, "FileWriter closeFile");
    if (this->hdf5_fileid_ >= 0) {
        assert(H5Fclose(this->hdf5_fileid_) >= 0);
        this->hdf5_fileid_ = 0;
    }
}

/**
 * Convert from a PixelType type to the corresponding HDF5 type.
 *
 * \param[in] pixel - The PixelType type to convert.
 * \return - the equivalent HDF5 type.
 */
hid_t FileWriter::pixelToHdfType(FileWriter::PixelType pixel) const {
    hid_t dtype = 0;
    switch(pixel)
    {
    case pixel_float32:
        dtype = H5T_NATIVE_UINT32;
        break;
    case pixel_raw_16bit:
        dtype = H5T_NATIVE_UINT16;
        break;
    case pixel_raw_8bit:
        dtype = H5T_NATIVE_UINT8;
        break;
    default:
        dtype = H5T_NATIVE_UINT16;
    }
    return dtype;
}

/**
 * Get a HDF5Dataset_t definition by its name.
 *
 * The private map of HDF5 dataset definitions is searched and i found
 * the HDF5Dataset_t definition is returned.  Throws a runtime error if
 * the dataset cannot be found.
 *
 * \param[in] dset_name - name of the dataset to search for.
 * \return - the dataset definition if found.
 */
FileWriter::HDF5Dataset_t& FileWriter::get_hdf5_dataset(const std::string dset_name) {
    // Check if the frame destination dataset has been created
    if (this->hdf5_datasets_.find(dset_name) == this->hdf5_datasets_.end())
    {
        // no dataset of this name exist
        LOG4CXX_ERROR(logger_, "Attempted to access non-existent dataset: \"" << dset_name << "\"\n");
        throw std::runtime_error("Attempted to access non-existent dataset");
    }
    return this->hdf5_datasets_.at(dset_name);
}

/**
 * Return the dataset offset for the supplied frame number.
 *
 * This method checks that the frame really belongs to this writer instance
 * in the case of multiple writer instances.  It then calculates the dataset
 * offset for this frame, which is the offset divided by the number of filewriter
 * concurrent processes.
 *
 * \param[in] frame_no - Frame number of the frame.
 * \return - the dataset offset for the frame number.
 */
size_t FileWriter::getFrameOffset(size_t frame_no) const {
    size_t frame_offset = this->adjustFrameOffset(frame_no);

    if (this->concurrent_processes_ > 1) {
        // Check whether this frame should really be in this process
        // Note: this expects the frame numbering from HW/FW to start at 1, not 0!
        if ( (((frame_no-1) % this->concurrent_processes_) - this->concurrent_rank_) != 0) {
            LOG4CXX_WARN(logger_, "Unexpected frame: " << frame_no
                                << " in this process rank: "
                                << this->concurrent_rank_);
            throw std::runtime_error("Unexpected frame in this process rank");
        }

        // Calculate the new offset based on how many concurrent processes are running
        frame_offset = frame_offset / this->concurrent_processes_;
    }
    return frame_offset;
}

/** Adjust the incoming frame number with an offset
 *
 * This is a hacky work-around a missing feature in the Mezzanine
 * firmware: the frame number is never reset and is ever incrementing.
 * The file writer can deal with it, by inserting the frame right at
 * the end of a very large dataset (fortunately sparsely written to disk).
 *
 * This function latches the first frame number and subtracts this number
 * from every incoming frame.
 *
 * Throws a std::range_error if a frame is received which has a smaller
 *   frame number than the initial frame used to set the offset.
 *
 * Returns the dataset offset for frame number (frame_no)
 */
size_t FileWriter::adjustFrameOffset(size_t frame_no) const {
    size_t frame_offset = 0;
    if (frame_no < this->start_frame_offset_) {
        // Deal with a frame arriving after the very first frame
        // which was used to set the offset: throw a range error
        throw std::range_error("Frame out of order at start causing negative file offset");
    }

    // Normal case: apply offset
    frame_offset = frame_no - this->start_frame_offset_;
    return frame_offset;
}

/** Part of big nasty work-around for the missing frame counter reset in FW
 *
 */
void FileWriter::setStartFrameOffset(size_t frame_no) {
    this->start_frame_offset_ = frame_no;
}

/** Extend the HDF5 dataset ready for new data
 *
 * Checks the frame_no is larger than the current dataset dimensions and then
 * sets the extent of the dataset to this new value.
 *
 * \param[in] dset - Handle to the HDF5 dataset.
 * \param[in] frame_no - Number of the incoming frame to extend to.
 */
void FileWriter::extend_dataset(HDF5Dataset_t& dset, size_t frame_no) const {
	herr_t status;
    if (frame_no > dset.dataset_dimensions[0]) {
        // Extend the dataset
        LOG4CXX_DEBUG(logger_, "Extending dataset_dimensions[0] = " << frame_no);
        dset.dataset_dimensions[0] = frame_no;
        status = H5Dset_extent( dset.datasetid,
                                &dset.dataset_dimensions.front());
        assert(status >= 0);
    }
}

/** Process an incoming frame.
 *
 * Checks we have been asked to write frames.  If we are in writing mode
 * then the frame is checked for subframes.  If subframes are found then
 * writeSubFrames is called.  If no subframes are found then writeFrame
 * is called.
 * Finally counters are updated and if the number of required frames has
 * been reached then the stopWriting method is called.
 *
 * \param[in] frame - Pointer to the Frame object.
 */
void FileWriter::processFrame(boost::shared_ptr<Frame> frame)
{
  // Protect this method
  boost::lock_guard<boost::recursive_mutex> lock(mutex_);

  if (writing_){

    // Check if the frame has defined subframes
    if (frame->has_parameter("subframe_count")){
      // The frame has subframes so write them out
      this->writeSubFrames(*frame);
    } else {
      // The frame has no subframes so write the whole frame
      this->writeFrame(*frame);
    }

    // Check if this is a master frame (for multi dataset acquisitions)
    // or if no master frame has been defined.  If either of these conditions
    // are true then increment the number of frames written.
    if (masterFrame_ == "" || masterFrame_ == frame->get_dataset_name()){
      framesWritten_++;
    }

    // Check if we have written enough frames and stop
    if (framesWritten_ == framesToWrite_){
      this->stopWriting();
    }
  }
}

/** Start writing frames to file.
 *
 * This method checks that the writer is not already writing.  Then it creates
 * the datasets required (from their definitions) and creates the HDF5 file
 * ready to write frames.  The framesWritten counter is reset to 0.
 */
void FileWriter::startWriting()
{
  if (!writing_){
    // Create the file
    this->createFile(filePath_ + fileName_);

    // Create the datasets from the definitions
    std::map<std::string, FileWriter::DatasetDefinition>::iterator iter;
    for (iter = this->dataset_defs_.begin(); iter != this->dataset_defs_.end(); ++iter){
      FileWriter::DatasetDefinition dset_def = iter->second;
      dset_def.num_frames = framesToWrite_;
      this->createDataset(dset_def);
    }

    // Reset counters
    framesWritten_ = 0;

    // Set writing flag to true
    writing_ = true;
  }
}

/** Stop writing frames to file.
 *
 * This method checks that the writer is currently writing.  Then it closes
 * the file and stops writing frames.
 */
void FileWriter::stopWriting()
{
  if (writing_){
    writing_ = false;
    this->closeFile();
  }
}

/**
 * Set configuration options for the file writer.
 *
 * This sets up the file writer plugin according to the configuration IpcMessage
 * objects that are received.  The options are searched for:
 * CONFIG_PROCESS - Calls the method processConfig
 * CONFIG_FILE - Calls the method fileConfig
 * CONFIG_DATASET - Calls the method dsetConfig
 *
 * Checks to see if the number of frames to write has been set.
 * Checks to see if the writer should start or stop writing frames.
 *
 * \param[in] config - IpcMessage containing configuration data.
 * \param[out] reply - Response IpcMessage.
 */
void FileWriter::configure(FrameReceiver::IpcMessage& config, FrameReceiver::IpcMessage& reply)
{
  // Protect this method
  boost::lock_guard<boost::recursive_mutex> lock(mutex_);

  LOG4CXX_DEBUG(logger_, config.encode());

  // Check to see if we are configuring the process number and rank
  if (config.has_param(FileWriter::CONFIG_PROCESS)){
    FrameReceiver::IpcMessage processConfig(config.get_param<const rapidjson::Value&>(FileWriter::CONFIG_PROCESS));
    this->configureProcess(processConfig, reply);
  }

  // Check to see if we are configuring the file path and name
  if (config.has_param(FileWriter::CONFIG_FILE)){
    FrameReceiver::IpcMessage fileConfig(config.get_param<const rapidjson::Value&>(FileWriter::CONFIG_FILE));
    this->configureFile(fileConfig, reply);
  }

  // Check to see if we are configuring a dataset
  if (config.has_param(FileWriter::CONFIG_DATASET)){
    FrameReceiver::IpcMessage dsetConfig(config.get_param<const rapidjson::Value&>(FileWriter::CONFIG_DATASET));
    this->configureDataset(dsetConfig, reply);
  }

  // Check to see if we are being told how many frames to write
  if (config.has_param(FileWriter::CONFIG_FRAMES)){
    framesToWrite_ = config.get_param<int>(FileWriter::CONFIG_FRAMES);
  }

  // Check to see if the master dataset is being set
  if (config.has_param(FileWriter::CONFIG_MASTER_DATASET)){
    masterFrame_ = config.get_param<std::string>(FileWriter::CONFIG_MASTER_DATASET);
  }

  // Final check is to start or stop writing
  if (config.has_param(FileWriter::CONFIG_WRITE)){
    if (config.get_param<bool>(FileWriter::CONFIG_WRITE) == true){
      this->startWriting();
    } else {
      this->stopWriting();
    }
  }
}

/**
 * Set configuration options for the file writer process count.
 *
 * This sets up the file writer plugin according to the configuration IpcMessage
 * objects that are received.  The options are searched for:
 * CONFIG_PROCESS_NUMBER - Sets the number of writer processes executing
 * CONFIG_PROCESS_RANK - Sets the rank of this process
 *
 * The configuration is not applied if the writer is currently writing.
 *
 * \param[in] config - IpcMessage containing configuration data.
 * \param[out] reply - Response IpcMessage.
 */
void FileWriter::configureProcess(FrameReceiver::IpcMessage& config, FrameReceiver::IpcMessage& reply)
{
  // If we are writing a file then we cannot change these items
  if (this->writing_){
    LOG4CXX_ERROR(logger_, "Cannot change concurrent processes or rank whilst writing");
    throw std::runtime_error("Cannot change concurrent processes or rank whilst writing");
  }

  // Check for process number and rank number
  if (config.has_param(FileWriter::CONFIG_PROCESS_NUMBER)){
    this->concurrent_processes_ = config.get_param<int>(FileWriter::CONFIG_PROCESS_NUMBER);
    LOG4CXX_DEBUG(logger_, "Concurrent processes changed to " << this->concurrent_processes_);
  }
  if (config.has_param(FileWriter::CONFIG_PROCESS_RANK)){
    this->concurrent_rank_ = config.get_param<int>(FileWriter::CONFIG_PROCESS_RANK);
    LOG4CXX_DEBUG(logger_, "Process rank changed to " << this->concurrent_rank_);
  }
}

/**
 * Set file configuration options for the file writer.
 *
 * This sets up the file writer plugin according to the configuration IpcMessage
 * objects that are received.  The options are searched for:
 * CONFIG_FILE_PATH - Sets the path of the file to write to
 * CONFIG_FILE_NAME - Sets the filename of the file to write to
 *
 * The configuration is not applied if the writer is currently writing.
 *
 * \param[in] config - IpcMessage containing configuration data.
 * \param[out] reply - Response IpcMessage.
 */
void FileWriter::configureFile(FrameReceiver::IpcMessage& config, FrameReceiver::IpcMessage& reply)
{
  // If we are writing a file then we cannot change these items
  if (this->writing_){
    LOG4CXX_ERROR(logger_, "Cannot change file path or name whilst writing");
    throw std::runtime_error("Cannot change file path or name whilst writing");
  }

  LOG4CXX_DEBUG(logger_, "Configure file name and path");
  // Check for file path and file name
  if (config.has_param(FileWriter::CONFIG_FILE_PATH)){
    this->filePath_ = config.get_param<std::string>(FileWriter::CONFIG_FILE_PATH);
    LOG4CXX_DEBUG(logger_, "File path changed to " << this->filePath_);
  }
  if (config.has_param(FileWriter::CONFIG_FILE_NAME)){
    this->fileName_ = config.get_param<std::string>(FileWriter::CONFIG_FILE_NAME);
    LOG4CXX_DEBUG(logger_, "File name changed to " << this->fileName_);
  }
}

/**
 * Set dataset configuration options for the file writer.
 *
 * This sets up the file writer plugin according to the configuration IpcMessage
 * objects that are received.  The options are searched for:
 * CONFIG_DATASET_CMD - Should we create/delete a dataset definition
 * CONFIG_DATASET_NAME - Name of the dataset
 * CONFIG_DATASET_TYPE - Datatype of the dataset
 * CONFIG_DATASET_DIMS - Dimensions of the dataset
 * CONFIG_DATASET_CHUNKS - Chunking parameters of the dataset
 *
 * The configuration is not applied if the writer is currently writing.
 *
 * \param[in] config - IpcMessage containing configuration data.
 * \param[out] reply - Response IpcMessage.
 */
void FileWriter::configureDataset(FrameReceiver::IpcMessage& config, FrameReceiver::IpcMessage& reply)
{
  // If we are writing a file then we cannot change these items
  if (this->writing_){
    LOG4CXX_ERROR(logger_, "Cannot update datasets whilst writing");
    throw std::runtime_error("Cannot update datasets whilst writing");
  }

  LOG4CXX_DEBUG(logger_, "Configure dataset");
  // Read the dataset command
  if (config.has_param(FileWriter::CONFIG_DATASET_CMD)){
    std::string cmd = config.get_param<std::string>(FileWriter::CONFIG_DATASET_CMD);

    // Command for creating a new dataset description
    if (cmd == "create"){
      FileWriter::DatasetDefinition dset_def;
      // There must be a name present for the dataset
      if (config.has_param(FileWriter::CONFIG_DATASET_NAME)){
        dset_def.name = config.get_param<std::string>(FileWriter::CONFIG_DATASET_NAME);
      } else {
        LOG4CXX_ERROR(logger_, "Cannot create a dataset without a name");
        throw std::runtime_error("Cannot create a dataset without a name");
      }

      // There must be a type present for the dataset
      if (config.has_param(FileWriter::CONFIG_DATASET_TYPE)){
        dset_def.pixel = (filewriter::FileWriter::PixelType)config.get_param<int>(FileWriter::CONFIG_DATASET_TYPE);
      } else {
        LOG4CXX_ERROR(logger_, "Cannot create a dataset without a data type");
        throw std::runtime_error("Cannot create a dataset without a data type");
      }

      // There must be dimensions present for the dataset
      if (config.has_param(FileWriter::CONFIG_DATASET_DIMS)){
        const rapidjson::Value& val = config.get_param<const rapidjson::Value&>(FileWriter::CONFIG_DATASET_DIMS);
        // Loop over the dimension values
        dimensions_t dims(val.Size());
        for (rapidjson::SizeType i = 0; i < val.Size(); i++){
          const rapidjson::Value& dim = val[i];
          dims[i] = dim.GetUint64();
        }
        dset_def.frame_dimensions = dims;
      } else {
        LOG4CXX_ERROR(logger_, "Cannot create a dataset without dimensions");
        throw std::runtime_error("Cannot create a dataset without dimensions");
      }

      // There might be chunking dimensions present for the dataset, this is not required
      if (config.has_param(FileWriter::CONFIG_DATASET_CHUNKS)){
        const rapidjson::Value& val = config.get_param<const rapidjson::Value&>(FileWriter::CONFIG_DATASET_CHUNKS);
        // Loop over the dimension values
        dimensions_t chunks(val.Size());
        for (rapidjson::SizeType i = 0; i < val.Size(); i++){
          const rapidjson::Value& dim = val[i];
          chunks[i] = dim.GetUint64();
        }
        dset_def.chunks = chunks;
      }

      LOG4CXX_DEBUG(logger_, "Creating dataset [" << dset_def.name << "] (" << dset_def.frame_dimensions[0] << ", " << dset_def.frame_dimensions[1] << ")");
      // Add the dataset definition to the store
      this->dataset_defs_[dset_def.name] = dset_def;
    }
  }
}

/**
 * Collate status information for the plugin.  The status is added to the status IpcMessage object.
 *
 * \param[out] status - Reference to an IpcMessage value to store the status.
 */
void FileWriter::status(FrameReceiver::IpcMessage& status)
{
  // Protect this method
  boost::lock_guard<boost::recursive_mutex> lock(mutex_);

  // Record the plugin's status items
  LOG4CXX_DEBUG(logger_, "File name " << this->fileName_);

  status.set_param(getName() + "/writing", this->writing_);
  status.set_param(getName() + "/frames_max", (int)this->framesToWrite_);
  status.set_param(getName() + "/frames_written", (int)this->framesWritten_);
  status.set_param(getName() + "/file_path", this->filePath_);
  status.set_param(getName() + "/file_name", this->fileName_);
  status.set_param(getName() + "/processes", (int)this->concurrent_processes_);
  status.set_param(getName() + "/rank", (int)this->concurrent_rank_);

  // Check for datasets
  std::map<std::string, FileWriter::DatasetDefinition>::iterator iter;
  for (iter = this->dataset_defs_.begin(); iter != this->dataset_defs_.end(); ++iter){
    // Add the dataset type
    status.set_param(getName() + "/datasets/" + iter->first + "/type", (int)iter->second.pixel);

    // Check for and add dimensions
    if (iter->second.frame_dimensions.size() > 0){
      std::string dimParamName = getName() + "/datasets/" + iter->first + "/dimensions[]";
      for (int index = 0; index < iter->second.frame_dimensions.size(); index++){
        status.set_param(dimParamName, (int)iter->second.frame_dimensions[index]);
      }
    }
    // Check for and add chunking dimensions
    if (iter->second.chunks.size() > 0){
      std::string chunkParamName = getName() + "/datasets/" + iter->first + "/chunks[]";
      for (int index = 0; index < iter->second.chunks.size(); index++){
        status.set_param(chunkParamName, (int)iter->second.chunks[index]);
      }
    }
  }
}

void FileWriter::hdfErrorHandler(unsigned n, const H5E_error2_t *err_desc)
{
  const int MSG_SIZE = 64;
  char maj[MSG_SIZE];
  char min[MSG_SIZE];
  char cls[MSG_SIZE];

  // Protect this method
  boost::lock_guard<boost::recursive_mutex> lock(mutex_);
printf("In here!!!\n");
  // Set the error flag true
  hdf5ErrorFlag_ = true;

  // Get descriptions for the major and minor error numbers
  H5Eget_class_name(err_desc->cls_id, cls, MSG_SIZE);
  H5Eget_msg(err_desc->maj_num, NULL, maj, MSG_SIZE);
  H5Eget_msg(err_desc->min_num, NULL, min, MSG_SIZE);

  // Record the error into the error stack
  std::stringstream err;
  err << "[" << cls << "] " << maj << " (" << min << ")";
  hdf5Errors_.push_back(err.str());
}

bool FileWriter::checkForHdfErrors()
{
  // Protect this method
  boost::lock_guard<boost::recursive_mutex> lock(mutex_);

  // Simply return the current error flag state
  return hdf5ErrorFlag_;
}

std::vector<std::string> FileWriter::readHdfErrors()
{
  // Protect this method
  boost::lock_guard<boost::recursive_mutex> lock(mutex_);

  // Simply return the current error array
  return hdf5Errors_;
}

void FileWriter::clearHdfErrors()
{
  // Protect this method
  boost::lock_guard<boost::recursive_mutex> lock(mutex_);

  // Empty the error array
  hdf5Errors_.clear();
  // Now reset the error flag
  hdf5ErrorFlag_ = false;
}

}
