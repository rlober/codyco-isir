/*! \file       IcubControllerClientManager.h
 *  \brief      Module class for the controller Client.
 *  \details
 *  \author     [Ryan Lober](http://www.ryanlober.com)
 *  \author     [Antoine Hoarau](http://ahoarau.github.io)
 *  \date       Feb 2016
 *  \copyright  GNU General Public License.
 */
/*
 *  This file is part of ocra-icub.
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

#include "ocra-icub/IcubControllerClientManager.h"

using namespace ocra_icub;

int IcubControllerClientManager::CONTROLLER_CLIENT_MANAGER_COUNT = 0;

IcubControllerClientManager::IcubControllerClientManager(IcubControllerClient::shared_ptr customClient)
{
    // Increment the module counter and save it in module number.
    moduleNumber = ++IcubControllerClientManager::CONTROLLER_CLIENT_MANAGER_COUNT;

    client = customClient;
    expectedClientPeriod = client->getExpectedPeriod();
}

IcubControllerClientManager::~IcubControllerClientManager()
{
    rpcPort.close();
}

std::string IcubControllerClientManager::getModuleName()
{
    return "ControllerClientModule_"+std::to_string(moduleNumber);
}

bool IcubControllerClientManager::configure(yarp::os::ResourceFinder &rf)
{
    return client->configure(rf);
}

int IcubControllerClientManager::launchClient()
{
    client->start();
    return runModule();
}

bool IcubControllerClientManager::interruptModule()
{
    if(client)
        client->suspend();
    return true;
}

bool IcubControllerClientManager::close()
{
    /* Stop the control thread. */
    if(client){
        client->stop();
    }

    /* Print performance information */
    printf("[PERFORMANCE INFORMATION]:\n");
    printf("Expected period %d ms.\nReal period: %3.1f+/-%3.1f ms.\n", expectedClientPeriod, avgTime, stdDev);
    printf("Real duration of 'run' method: %3.1f+/-%3.1f ms.\n", avgTimeUsed, stdDevUsed);
    if(avgTime<0.5*(double)expectedClientPeriod)
        printf("Next time you could set a lower period to improve the controller performance.\n");
    else if(avgTime>1.3*(double)expectedClientPeriod)
        printf("The period you set was impossible to attain. Next time you could set a higher period.\n");
    return true;
}

bool IcubControllerClientManager::updateModule()
{
    // Get the average time between two calls of the Rate.run() method.
    client->getEstPeriod(avgTime, stdDev);

    // Get the average time the .run() method takes to compute the control.
    client->getEstUsed(avgTimeUsed, stdDevUsed);

    // If the period of the control thread is too slow then print a warning.
    if(avgTime > 1.3*(double)expectedClientPeriod)
    {
        yLog.warning() << "CLIENT THREAD LOOP IS TOO SLOW\nReal period: "<< avgTime <<"+/-"<< stdDev <<"\nExpected period: " << expectedClientPeriod <<"\nDuration of 'run' method: "<<avgTimeUsed<<"+/-"<< stdDevUsed<<"\n";
    }

    return customUpdateModule();
}

void IcubControllerClientManager::printHelp()
{
    std::cout << "Hey there this is the help for the IcubControllerClientManager. Here is what the client is saying..." << std::endl;
    client->printHelp();
}

void IcubControllerClientManager::callbackParser(yarp::os::Bottle& message, yarp::os::Bottle& reply)
{
    if (message.size() != 0) {
        reply.clear();
        customCallbackParser(message, reply);
    }
}

void IcubControllerClientManager::customCallbackParser(yarp::os::Bottle& message, yarp::os::Bottle& reply)
{
    // Do nothing if not implemented.
}
bool IcubControllerClientManager::customUpdateModule()
{
    // Do nothing if not implemented.
    return true;
}


/**************************************************************************************************
                                    Nested moduleCallback Class
**************************************************************************************************/
IcubControllerClientManager::moduleCallback::moduleCallback(IcubControllerClientManager& newModuleRef)
: moduleRef(newModuleRef)
{
    //do nothing
}

bool IcubControllerClientManager::moduleCallback::read(yarp::os::ConnectionReader& connection)
{
    yarp::os::Bottle input, reply;

    if (!input.read(connection)){
        return false;
    }
    else{
        moduleRef.callbackParser(input, reply);
        yarp::os::ConnectionWriter* returnToSender = connection.getWriter();
        if (returnToSender!=NULL) {
            reply.write(*returnToSender);
        }
        return true;
    }
}
/**************************************************************************************************
**************************************************************************************************/