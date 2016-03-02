/*! \file       OcraControllerServerThread.cpp
 *  \brief      The thread class for the controller server.
 *  \details
 *  \author     [Ryan Lober](http://www.ryanlober.com)
 *  \author     [Antoine Hoarau](http://ahoarau.github.io)
 *  \date       Feb 2016
 *  \copyright  GNU General Public License.
 */
/*
 *  This file is part of ocra-yarp.
 *  Copyright (C) 2016 Institut des Systèmes Intelligents et de Robotique (ISIR)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ocra-yarp/OcraControllerServerThread.h"



using namespace ocra_yarp;

OcraControllerOptions::OcraControllerOptions()
: threadPeriod(10)
, serverName("")
, robotName("")
, startupTaskSetPath("")
, startupSequence("")
, wbiConfigFilePath("")
, runInDebugMode(false)
, isFloatingBase(false)
, yarpWbiOptions(yarp::os::Property())
, controllerType(WOCRA_CONTROLLER)
{
}

OcraControllerOptions::~OcraControllerOptions()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
//                                               Thread Constructor
//
////////////////////////////////////////////////////////////////////////////////////////////////////////////
OcraControllerServerThread::OcraControllerServerThread(OcraControllerOptions& controller_options, std::shared_ptr<wbi::wholeBodyInterface> wbi)
: RateThread(controller_options.threadPeriod)
, ctrlOptions(controller_options)
, isStabilizing(false)
, controllerStatus(OCRA_CONTROLLER_MESSAGE::CONTROLLER_STOPPED)
, taskSequence(NULL)
, ocraModel(NULL)
, ctrl(NULL)
{
    bool useReducedProblem = false;

    robot           = wbi;
    ocraModel       = new OcraWbiModel(ctrlOptions.robotName, robot->getDoFs(), robot, ctrlOptions.isFloatingBase);
    ctrl            = new wocra::WocraController("icubControl", *ocraModel, internalSolver, useReducedProblem);
    modelUpdater    = std::make_shared<OcraWbiModelUpdater>();

    homePosture     = Eigen::VectorXd::Zero(robot->getDoFs());
    debugPosture    = Eigen::VectorXd::Zero(robot->getDoFs());
    initialPosture  = Eigen::VectorXd::Zero(robot->getDoFs());
    torques         = Eigen::VectorXd::Zero(robot->getDoFs());
    measuredTorques = Eigen::VectorXd::Zero(robot->getDoFs());
    refSpeed        = Eigen::VectorXd::Constant(robot->getDoFs(), REFERENCE_JOINT_VELOCITY);
    minTorques      = Eigen::ArrayXd::Constant(robot->getDoFs(), TORQUE_MIN);
    maxTorques      = Eigen::ArrayXd::Constant(robot->getDoFs(), TORQUE_MAX);

    getHomePosture(*ocraModel, homePosture);
    getNominalPosture(*ocraModel, debugPosture);

    time_sim = 0;

}

OcraControllerServerThread::~OcraControllerServerThread()
{
    // if(taskSequence!=NULL){delete(taskSequence);}
    // if(ctrl!=NULL){delete(ctrl);}
    // if(ocraModel!=NULL){delete(ocraModel);}
    // // rpcServerPort.close();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
//                                               Thread Init
//
////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool OcraControllerServerThread::threadInit()
{
    // Construct rpc server callback and bind to the control thread.
    rpcServerCallback = std::make_shared<ControllerRpcServerCallback>(*this);
    // rpcServerCallback = ControllerRpcServerCallback(*this)
    // Open the rpc server port.
    rpcServerPort.open("/OCRA/Controller/rpc:i");
    // Bind the callback to the port.
    rpcServerPort.setReader(*rpcServerCallback);


    // /******************************************************************************************************
    //                             Get WBI estimates and initialize the WOCRA model
    // ******************************************************************************************************/

    if(!modelUpdater->initialize(robot, ocraModel))
        std::cout << "ERROR with model updater" << std::endl;


    robot->getEstimates(ESTIMATE_JOINT_POS, initialPosture.data(), ALL_JOINTS);
    initialCoMPosition = ocraModel->getCoMPosition();
    initialTorsoPosition = ocraModel->getSegmentPosition(ocraModel->getSegmentIndex("torso")).getTranslation();

    /******************************************************************************************************
                                        Parse tasks and load sequence
    ******************************************************************************************************/




    if (ctrlOptions.runInDebugMode)
    {
        if (ocraModel->hasFixedRoot()) {
            std::cout << "Loading fixed base minimal tasks..." << std::endl;
            // taskSequence = LoadSequence("Debug");
            std::cout << "\n\n\t------------------------------" << std::endl;
            std::cout << "\t  Running in DEBUG mode..." << std::endl;
            std::cout << "\t------------------------------\n" << std::endl;

            std::string portPrefix = "/OCRA/Controller/debug";
            debugPort_in.open((portPrefix+":i").c_str());
            debugPort_out.open((portPrefix+":o").c_str());
            debugJointIndex = 3;
        }
        else{
            std::cout << "[ERR] Cannot run debug mode with floating base." << std::endl;
            return false;
        }

    }
    else
    {
        std::cout << "\n\n=== Creating wocraController ===" << std::endl;
        // rpcPort.open("/OcraController/rpc:i");
        // rpcPort.setReader(processor);

        //Create cpp sequence
        if (!ctrlOptions.startupSequence.empty()) {
            std::cout << "\nLoading sequence:\n" << ctrlOptions.startupSequence << "\n" << std::endl;
            // taskSequence = LoadSequence(ctrlOptions.startupSequence);
        }

        //Create XML task set
        if (!ctrlOptions.startupTaskSetPath.empty()) {
            if (ctrlOptions.startupSequence.empty()) {
                taskSequence = new ocra::TaskSequence();
            }
            std::cout << "\nLoading tasks from XML file:\n" << ctrlOptions.startupTaskSetPath << "\n" << std::endl;
            ocra::TaskParser taskParser;
            if(taskParser.parseTasksXML( ctrlOptions.startupTaskSetPath.c_str() )){
                taskParser.addTaskManagersToSequence(*ctrl, *ocraModel, taskSequence);
                // taskParser.printTaskArguments(); // If you want to see all the parsed args.
            }
            else{
                //TODO: Implement fall pack procedure for failure to parse xml tasks.;
            }
        }
        else{
            std::cout << "No XML task set detected." << std::endl;
        }


        if ( (ctrlOptions.startupTaskSetPath.empty()) && (ctrlOptions.startupSequence.empty()) )
        {
            std::cout << "\nNo tasks or scenarios loaded on startup. Defaulting to standard initial tasks." << std::endl;
            if (!ocraModel->hasFixedRoot()) {
                std::cout << "Loading floating base minimal tasks..." << std::endl;
                // taskSequence = LoadSequence("FloatingBaseMinimalTasks");
            }
            else{
                std::cout << "Loading fixed base minimal tasks..." << std::endl;
                // taskSequence = LoadSequence("FixedBaseMinimalTasks");
            }
        }

    }



    /******************************************************************************************************
                                            Initialize sequence
    ******************************************************************************************************/


    taskSequence->init(*ctrl, *ocraModel);


    /******************************************************************************************************
                                    Set the control mode of the Robot
    ******************************************************************************************************/

    // Note: We do this after taskSequence->init so that if the task sequence takes a long time to start up, the robot will not fall over because its control mode is already set to torque but it is receiving 0 values.

    if (ctrlOptions.runInDebugMode)
    {
        robot->setControlMode(CTRL_MODE_POS, debugPosture.data(), ALL_JOINTS);
        robot->setControlReference(debugPosture.data());
        robot->setControlMode(CTRL_MODE_TORQUE, 0, debugJointIndex);
    }
    else
    {
        // Set all declared joints in module to TORQUE mode
        bool res_setControlMode = robot->setControlMode(CTRL_MODE_TORQUE, 0, ALL_JOINTS);
    }

    controllerStatus = OCRA_CONTROLLER_MESSAGE::CONTROLLER_RUNNING;

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
//                                               Thread Run
//
////////////////////////////////////////////////////////////////////////////////////////////////////////////

void OcraControllerServerThread::run()
{
    /******************************************************************************************************
                                            Update dynamic model
    ******************************************************************************************************/

    if(!modelUpdater->update(robot, ocraModel))
        std::cout << "ERROR with model updater" << std::endl;


    /******************************************************************************************************
                                            Update task sequences
    ******************************************************************************************************/

    if (ctrlOptions.runInDebugMode)
    {
        yarp::os::Bottle *input = debugPort_in.read(false);
        if (input != NULL)
        {
            int tempDebugIndex = input->get(0).asInt();
            if (tempDebugIndex>=0 && tempDebugIndex<robot->getDoFs()) {
                std::cout << "\n-----\nNew joint received...\n" << std::endl;
                std::cout << "Returning joint: " << debugJointIndex << " to home position." << std::endl;
                robot->setControlMode(CTRL_MODE_POS, &debugPosture[debugJointIndex], debugJointIndex);
                robot->setControlReference(&debugPosture[debugJointIndex], debugJointIndex);

                debugJointIndex = tempDebugIndex;
                if(robot->setControlMode(CTRL_MODE_TORQUE, &torques[debugJointIndex], debugJointIndex) )
                {
                    std::cout << "Now joint: " << debugJointIndex << " is now being tested in torque control.\n-----\n" << std::endl;
                }
            }
            else{std::cout << "\n[WARNING] (thread.run) The command you sent was not a valid joint index. Please use integers between 0 and "<< robot->getDoFs() - 1 << ".\n"<< std::endl;}

        }
    }
    else if (!isStabilizing){
        taskSequence->update(time_sim, *ocraModel, NULL);
    }


    /******************************************************************************************************
                                Compute desired torque by calling the controller
    ******************************************************************************************************/

	ctrl->computeOutput(torques);


    /******************************************************************************************************
                                        Threshold the computed torques
    ******************************************************************************************************/

    torques = ((torques.array().max(minTorques)).min(maxTorques)).matrix().eval();


    /******************************************************************************************************
                                    Send the torques to the robot via WBI
    ******************************************************************************************************/

    if (ctrlOptions.runInDebugMode) {
        robot->setControlReference(&torques[debugJointIndex], debugJointIndex);
    }
    else{
        robot->setControlReference(torques.data());
    }


    // /******************************************************************************************************
    //                         Print out useful information at every "printPeriod" (ms)
    // ******************************************************************************************************/
    // printPeriod = 500;
    // printCountdown = (printCountdown>=printPeriod) ? 0 : printCountdown + getRate(); // countdown for next print
    // if (printCountdown == 0)
    // {
    //     if (ctrlOptions.runInDebugMode)
    //     {
    //         bool res_torque = robot->getEstimates(ESTIMATE_JOINT_TORQUE, measuredTorques.data(), ALL_JOINTS);
    //         yarp::os::Bottle& output = debugPort_out.prepare();
    //         output.clear();
    //
    //         output.addString("joint");
    //         output.addInt(debugJointIndex);
    //         output.addString("torque_desired");
    //         output.addDouble(torques[debugJointIndex]);
    //         output.addString("torque_measured");
    //         output.addDouble(measuredTorques[debugJointIndex]);
    //
    //         debugPort_out.write();
    //     }
    //     else if (ocraModel->hasFixedRoot()){
    //         // Put whatever you want to print here for fixed base...
    //     }
    //     else if (!ocraModel->hasFixedRoot()){
    //         // Put whatever you want to print here for floating base...
    //     }
    //
    // }

    time_sim += TIME_MSEC_TO_SEC * getRate();
}

void OcraControllerServerThread::threadRelease()
{
    taskSequence->clearSequence();

    if (!ocraModel->hasFixedRoot()){
        if(loadStabilizationTasks()){
            stabilizeRobot();
        }else{
            std::cout << "[WARNING] Error loading stabilization task set. Could not perform safe stabilization procedure." << std::endl;
        }
    }

    if(robot->setControlMode(CTRL_MODE_POS, 0, ALL_JOINTS) ){
        bool res_setControlReference = robot->setControlReference(initialPosture.data());
        std::cout << "\n\n--> Closing controller thread. Switching to POSITION mode and returning to home pose.\n" << std::endl;
        taskSequence->clearSequence();
        controllerStatus = OCRA_CONTROLLER_MESSAGE::CONTROLLER_STOPPED;

    }
    else{
        std::cout << "[ERROR] (OcraControllerServerThread::threadRelease): Could not set the robot into Position Control mode." << std::endl;
    }
}

void OcraControllerServerThread::parseIncomingMessage(yarp::os::Bottle& input, yarp::os::Bottle& reply)
{
    int btlSize = input.size();
    for (int i=0; i<btlSize;)
    {
        // OCRA_CONTROLLER_MESSAGE message();
        switch (input.get(i).asInt()) {
            case GET_CONTROLLER_STATUS:
            i++;
            std::cout << "Got message: GET_CONTROLLER_STATUS." << std::endl;
            reply.addInt(controllerStatus);
            break;
            case GET_WBI_CONFIG_FILE_PATH:
            i++;
            std::cout << "Got message: GET_WBI_CONFIG_FILE_PATH." << std::endl;
            reply.addString(ctrlOptions.wbiConfigFilePath);
            break;
            case GET_ROBOT_NAME:
            i++;
            std::cout << "Got message: GET_ROBOT_NAME." << std::endl;
            reply.addString(ctrlOptions.robotName);
            break;
            case START_CONTROLLER:
            i++;
            std::cout << "Got message: START_CONTROLLER." << std::endl;
            break;
            case STOP_CONTROLLER:
            i++;
            std::cout << "Got message: STOP_CONTROLLER." << std::endl;
            break;
            case PAUSE_CONTROLLER:
            i++;
            std::cout << "Got message: PAUSE_CONTROLLER." << std::endl;
            break;
            case ADD_TASK:
            i++;
            std::cout << "Got message: ADD_TASK." << std::endl;
            break;
            case ADD_TASK_FROM_FILE:
            i++;
            std::cout << "Got message: ADD_TASK_FROM_FILE." << std::endl;
            break;
            case REMOVE_TASK:
            i++;
            std::cout << "Got message: REMOVE_TASK." << std::endl;
            break;
            case REMOVE_TASKS:
            i++;
            std::cout << "Got message: REMOVE_TASKS." << std::endl;
            break;
            case GET_TASK_LIST:
            i++;
            std::cout << "Got message: GET_TASK_LIST." << std::endl;
            break;
            case GET_TASK_PORT_LIST:
            i++;
            std::cout << "Got message: GET_TASK_PORT_LIST." << std::endl;
            break;
            case HELP:
            i++;
            std::cout << "Got message: HELP." << std::endl;
            break;
            default:
            i++;
            std::cout << "Got message: UNKNOWN." << std::endl;
            break;
        }
    }
}
//
//         // std::string msgTag = input->get(i).asString();
//         //
//         // if(msgTag == "addTaskYARP")
//         // {
//         //     std::cout << "Implement yarp task parsing" << std::endl;
//         //     i++;
//         // }
//         //
//         // // Stiffness
//         // else if(msgTag == "addTaskXML")
//         // {
//         //     i++;
//         //     ocra::TaskParser taskParser;
//         //     taskParser.parseTasksXML( input->get(i).asString().c_str() );
//         //     taskParser.addTaskManagersToSequence(*ctrl, *ocraModel, taskSequence);
//         //     i++;
//         // }
//         // else if(msgTag == "removeTask")
//         // {
//         //     i++;
//         //     std::string taskToRemove = input->get(i).asString();
//         //     taskSequence->removeTaskManager(taskToRemove);
//         //     ctrl->removeTask(taskToRemove);
//         //     reply->addString("Removed:");
//         //     reply->addString(taskToRemove);
//         //     i++;
//         // }
//         //
//         // else if(msgTag == "getTaskList")
//         // {
//         //     reply->addString("tasks:");
//         //     std::vector<std::string> strVector = taskSequence->getTaskList();
//         //     for(int j=0; j<strVector.size(); j++){
//         //         reply->addString(strVector[j]);
//         //     }
//         //     i++;
//         // }
//         //
//         // else if(msgTag == "getTaskPorts")
//         // {
//         //     reply->addString("taskPorts:");
//         //     std::vector<std::string> strVector = taskSequence->getTaskPorts();
//         //     for(int j=0; j<strVector.size(); j++){
//         //         reply->addString(strVector[j]);
//         //     }
//         //     i++;
//         // }
//         //
//         // else if (msgTag == "help")
//         // {
//         //     // TODO: Properly print help message to rpc reply
//         //     // reply->addString(printValidMessageTags());
//         //     std::cout << printValidMessageTags();
//         //     i++;
//         // }
//         //
//         // // Fallback
//         // else
//         // {
//         //     std::cout << "[WARNING] (OcraControllerServerThread::parseIncomingMessage): The message tag, " << msgTag << " doesn't exist. Skipping. Use help to see availible options." << std::endl;
//         //
//         //     reply->addString("invalid_input");
//         //     i++;
//         // }
// //     }
// // }
//
// std::string OcraControllerServerThread::printValidMessageTags()
// {
//     std::string helpString  = "\n=== Valid message tags are: ===\n";
//     helpString += "addTaskYARP: NOT IMPLEMENTED.\n";
//     helpString += "addTaskXML: Add task(s) to the task sequence from an xml file.\n";
//     helpString += "removeTask: Allows you to remove a single task manager from the sequence.\n";
//     helpString += "getTaskList: Gets a list of all current tasks.\n";
//     helpString += "getTaskPorts: Gets a list of all current task ports.\n";
//     helpString += "help: Prints this message you just read.\n";
//     return helpString;
// }
//
bool OcraControllerServerThread::loadStabilizationTasks()
{
    ocra::TaskParser taskParser;
    yarp::os::ResourceFinder RF;
    std::string filePath = RF.findFileByName("taskSets/stabilizationTaskSet.xml");
    taskParser.parseTasksXML(filePath.c_str());
    return taskParser.addTaskManagersToSequence(*ctrl, *ocraModel, taskSequence);
}

void OcraControllerServerThread::stabilizeRobot()
{
    isStabilizing = true; // prevents sequences from being updated in the run loop.

    std::string postureTaskKey = "stabilization_fullPosture";
    std::string comTaskKey = "stabilization_comTask";
    std::string torsoTaskKey = "stabilization_torsoCartesianTask";

    (std::dynamic_pointer_cast<ocra::FullPostureTaskManager>(taskSequence->getTaskManagerPointer(postureTaskKey)))->setPosture(initialPosture);
    (std::dynamic_pointer_cast<ocra::CoMTaskManager>(taskSequence->getTaskManagerPointer(comTaskKey)))->setState(initialCoMPosition);
    (std::dynamic_pointer_cast<ocra::SegCartesianTaskManager>(taskSequence->getTaskManagerPointer(torsoTaskKey)))->setState(initialTorsoPosition);


    double timeStabilizingStart = yarp::os::Time::now();
    double timeStabilizing = 0.0;
    const double STABILIZATION_TIMEOUT = 1.0;


    std::cout << "Attempting to stabilize the robot's posture. \nTime elapsed:" << std::endl;

    do{
        run();
        timeStabilizing = yarp::os::Time::now() - timeStabilizingStart;

        if (std::fmod(timeStabilizing, 5.0) <= 0.01) {
            // Every 5 seconds or so print the time.
            std::cout << std::setprecision(4) << timeStabilizing << " sec" << std::endl;
        }
        if (timeStabilizing >= STABILIZATION_TIMEOUT) {
            std::cout << "\n****\n[WARNING] Stabilization procedure has timed out. The robot may fall!\n****\n";
        }
    }while(!isRobotStable() && timeStabilizing < STABILIZATION_TIMEOUT);

    if(isRobotStable())
    {
        std::cout << "Stabilization procedure complete!" << std::endl;
    }
}

bool OcraControllerServerThread::isRobotStable()
{
    const double ZERO_VELOCITY_THRESHOLD = 0.01;
    return (ocraModel->getJointVelocities().norm()) <= ZERO_VELOCITY_THRESHOLD;
}


/**************************************************************************************************
                                    Nested PortReader Class
**************************************************************************************************/
// ControllerRpcServerCallback::ControllerRpcServerCallback(OcraControllerServerThread::shared_ptr ctThreadPtr)
OcraControllerServerThread::ControllerRpcServerCallback::ControllerRpcServerCallback(OcraControllerServerThread& ctThreadPtr)
: ctThread(ctThreadPtr)
{
    //do nothing
}

bool OcraControllerServerThread::ControllerRpcServerCallback::read(yarp::os::ConnectionReader& connection)
{
    yarp::os::Bottle input, reply;

    if (!input.read(connection)){
        return false;
    }
    else{
        ctThread.parseIncomingMessage(input, reply);
        yarp::os::ConnectionWriter* returnToSender = connection.getWriter();
        if (returnToSender!=NULL) {
            reply.write(*returnToSender);
        }
        return true;
    }
}
/**************************************************************************************************
**************************************************************************************************/
